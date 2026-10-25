#include <SDL.h>
#include <SDL_image.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

struct StairEnd { std::string floorId; int x = 0; int y = 0; };
struct Stair { std::string id; StairEnd ends[2]; };
struct Floor {
    std::string id;
    int width = 0;
    int height = 0;
    std::unordered_map<std::uint64_t, std::string> hEdges;
    std::unordered_map<std::uint64_t, std::string> vEdges;
};
struct MapData {
    int version = 1;
    std::vector<Floor> floors;
    std::vector<Stair> stairs;
    StairEnd spawn;
};

std::uint64_t edgeKey(int x, int y) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32)
        | static_cast<std::uint32_t>(y);
}

const Floor* findFloor(const MapData& map, const std::string& id) {
    for (const Floor& floor : map.floors) if (floor.id == id) return &floor;
    return nullptr;
}

MapData loadMap(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open: " + path.string());
    json document;
    in >> document;
    if (document.value("version", 0) != 1) throw std::runtime_error("Unsupported map version");

    MapData map;
    for (const auto& fj : document.at("floors")) {
        Floor floor;
        floor.id = fj.at("id").get<std::string>();
        floor.width = fj.at("width").get<int>();
        floor.height = fj.at("height").get<int>();
        for (const auto& ej : fj.value("edges", json::array())) {
            std::string axis = ej.at("axis").get<std::string>();
            std::string type = ej.at("type").get<std::string>();
            int x = ej.at("locate").at(0).get<int>();
            int y = ej.at("locate").at(1).get<int>();
            int length = ej.value("length", 1);
            for (int offset = 0; offset < length; ++offset) {
                if (axis == "h") floor.hEdges[edgeKey(x + offset, y)] = type;
                else floor.vEdges[edgeKey(x, y + offset)] = type;
            }
        }
        map.floors.push_back(std::move(floor));
    }
    for (const auto& sj : document.value("stairs", json::array())) {
        Stair stair;
        stair.id = sj.at("id").get<std::string>();
        for (int i = 0; i < 2; ++i) {
            stair.ends[i].floorId = sj.at("ends").at(i).at("floorId").get<std::string>();
            stair.ends[i].x = sj.at("ends").at(i).at("locate").at(0).get<int>();
            stair.ends[i].y = sj.at("ends").at(i).at("locate").at(1).get<int>();
        }
        map.stairs.push_back(stair);
    }
    if (document.contains("spawn")) {
        map.spawn.floorId = document["spawn"].at("floorId").get<std::string>();
        map.spawn.x = document["spawn"].at("locate").at(0).get<int>();
        map.spawn.y = document["spawn"].at("locate").at(1).get<int>();
    } else if (!map.floors.empty()) {
        map.spawn.floorId = map.floors.front().id;
    }
    return map;
}

struct TextureAsset {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
};

bool isImageFile(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return extension == ".png" || extension == ".bmp" || extension == ".jpg" || extension == ".jpeg" || extension == ".webp";
}

fs::path findAsset(const fs::path& directory, const std::string& preferredName) {
    if (!fs::exists(directory) || !fs::is_directory(directory)) return {};
    fs::path preferred = directory / preferredName;
    if (fs::exists(preferred) && isImageFile(preferred)) return preferred;
    for (fs::path candidate : directory) {
        if (candidate.is_relative()) candidate = directory / candidate;
    }
    std::vector<fs::path> matches;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && isImageFile(entry.path())) matches.push_back(entry.path());
    }
    std::sort(matches.begin(), matches.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    return matches.empty() ? fs::path{} : matches.front();
}

TextureAsset loadTexture(SDL_Renderer* renderer, const fs::path& path) {
    if (path.empty()) return {};
    SDL_Texture* texture = IMG_LoadTexture(renderer, path.string().c_str());
    if (!texture) return {};
    int width = 0;
    int height = 0;
    SDL_QueryTexture(texture, nullptr, nullptr, &width, &height);
    return {texture, width, height};
}


constexpr int kViewWidth = 960;
constexpr int kViewHeight = 600;
constexpr double kPi = 3.14159265358979323846;
// カメラ平面の半幅。1.0 で水平視野角 90°（現在セルの左右の壁が画面端に入る）
constexpr double kPlane = 1.0;
// カメラをセル中心から後方へずらす量。0.5 でセル後端 = Wizardry/BlackOnyx 風の視点
constexpr double kCameraBack = 0.5;
constexpr double kMoveDuration = 0.20;
constexpr double kTurnDuration = 0.18;
constexpr double kTurnBackDuration = 0.30;

// facing: 0=東(+x) 1=南(+y) 2=西(-x) 3=北(-y)。画面座標系(y下向き)なので角度 = facing * 90°
constexpr int kDirX[4] = {1, 0, -1, 0};
constexpr int kDirY[4] = {0, 1, 0, -1};

struct Hit {
    bool hit = false;
    std::string type;
    bool vertical = false;   // x 軸に垂直な辺(vEdge)に当たった
    int edgeX = 0;
    int edgeY = 0;
    double distance = 0;     // カメラ平面からの垂直距離
    double wallX = 0;
};

std::string hEdge(const Floor& floor, int x, int y) {
    if (y < 0 || y > floor.height) return "wall";
    auto found = floor.hEdges.find(edgeKey(x, y));
    return found == floor.hEdges.end() ? "" : found->second;
}

std::string vEdge(const Floor& floor, int x, int y) {
    if (x < 0 || x > floor.width) return "wall";
    auto found = floor.vEdges.find(edgeKey(x, y));
    return found == floor.vEdges.end() ? "" : found->second;
}

// rayDirection は正規化しない (direction + plane * camera)。このとき DDA の距離がそのまま垂直距離になる
Hit castRay(const Floor& floor, double originX, double originY, int mapX, int mapY,
            double rayDirectionX, double rayDirectionY) {
    const double deltaX = rayDirectionX == 0 ? 1e30 : std::abs(1.0 / rayDirectionX);
    const double deltaY = rayDirectionY == 0 ? 1e30 : std::abs(1.0 / rayDirectionY);
    int stepX = rayDirectionX < 0 ? -1 : 1;
    int stepY = rayDirectionY < 0 ? -1 : 1;
    double sideX = rayDirectionX < 0
        ? (originX - mapX) * deltaX
        : (mapX + 1.0 - originX) * deltaX;
    double sideY = rayDirectionY < 0
        ? (originY - mapY) * deltaY
        : (mapY + 1.0 - originY) * deltaY;

    for (int step = 0; step < 128; ++step) {
        bool verticalSide = false;
        double rawDistance = 0.0;
        if (sideX < sideY) {
            mapX += stepX;
            rawDistance = sideX;
            sideX += deltaX;
            verticalSide = true;
        } else {
            mapY += stepY;
            rawDistance = sideY;
            sideY += deltaY;
        }
        bool outside = mapX < 0 || mapX >= floor.width || mapY < 0 || mapY >= floor.height;

        Hit hit;
        hit.vertical = verticalSide;
        if (verticalSide) {
            hit.edgeX = stepX > 0 ? mapX : mapX + 1;
            hit.edgeY = mapY;
            hit.type = vEdge(floor, hit.edgeX, hit.edgeY);
            hit.wallX = originY + rawDistance * rayDirectionY;
            hit.wallX -= std::floor(hit.wallX);
            if (stepX < 0) hit.wallX = 1.0 - hit.wallX;
        } else {
            hit.edgeX = mapX;
            hit.edgeY = stepY > 0 ? mapY : mapY + 1;
            hit.type = hEdge(floor, hit.edgeX, hit.edgeY);
            hit.wallX = originX + rawDistance * rayDirectionX;
            hit.wallX -= std::floor(hit.wallX);
            if (stepY > 0) hit.wallX = 1.0 - hit.wallX;
        }
        if (hit.type.empty() && outside) hit.type = "wall";
        if (!hit.type.empty()) {
            hit.hit = true;
            hit.distance = std::max(rawDistance, 0.001);
            return hit;
        }
    }
    return {};
}

bool moveBlocked(const Floor& floor, int cellX, int cellY, int stepX, int stepY) {
    if (cellX < 0 || cellX >= floor.width || cellY < 0 || cellY >= floor.height) return true;
    int targetX = cellX + stepX;
    int targetY = cellY + stepY;
    if (targetX < 0 || targetX >= floor.width || targetY < 0 || targetY >= floor.height) return true;

    if (stepX > 0) {
        std::string edge = vEdge(floor, targetX, cellY);
        return !edge.empty() && edge != "door";
    }
    if (stepX < 0) {
        std::string edge = vEdge(floor, cellX, cellY);
        return !edge.empty() && edge != "door";
    }
    if (stepY > 0) {
        std::string edge = hEdge(floor, cellX, targetY);
        return !edge.empty() && edge != "door";
    }
    if (stepY < 0) {
        std::string edge = hEdge(floor, cellX, cellY);
        return !edge.empty() && edge != "door";
    }
    return false;
}

// 距離による減光 (遠くほど闇に沈む)
double distanceShade(double distance) {
    return std::clamp(1.1 - distance * 0.16, 0.08, 1.0);
}

Uint8 shadeChannel(int value, double shade) {
    return static_cast<Uint8>(std::clamp(static_cast<int>(value * shade), 0, 255));
}

void drawPseudo3D(SDL_Renderer* renderer, const Floor& floor,
                  const TextureAsset& wallTexture, const TextureAsset& doorTexture,
                  double viewX, double viewY, double angle) {
    const int width = kViewWidth;
    const int height = kViewHeight;
    const int horizon = height / 2;
    // 距離 1 の壁 1 マス分が画面上で何ピクセルになるか
    const double projection = (width / 2.0) / kPlane;

    double directionX = std::cos(angle);
    double directionY = std::sin(angle);
    if (std::abs(directionX) < 1e-9) directionX = 0.0;
    if (std::abs(directionY) < 1e-9) directionY = 0.0;
    const double planeX = -directionY * kPlane;
    const double planeY = directionX * kPlane;

    const double cameraX = viewX - directionX * kCameraBack;
    const double cameraY = viewY - directionY * kCameraBack;
    // カメラはセル境界上に乗ることがあるので、前方へ微小にずらした位置で所属セルを決める
    const int startCellX = static_cast<int>(std::floor(cameraX + directionX * 1e-6));
    const int startCellY = static_cast<int>(std::floor(cameraY + directionY * 1e-6));

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    // 床・天井: 行ごとの距離で減光したグラデーション
    for (int row = 1; row <= horizon; ++row) {
        double rowDistance = projection / (2.0 * (row - 0.5));
        double shade = distanceShade(rowDistance);
        SDL_SetRenderDrawColor(renderer, shadeChannel(70, shade), shadeChannel(62, shade), shadeChannel(52, shade), 255);
        SDL_RenderDrawLine(renderer, 0, horizon + row - 1, width - 1, horizon + row - 1);
        SDL_SetRenderDrawColor(renderer, shadeChannel(40, shade), shadeChannel(40, shade), shadeChannel(46, shade), 255);
        SDL_RenderDrawLine(renderer, 0, horizon - row, width - 1, horizon - row);
    }

    struct Column {
        bool hit = false;
        std::uint64_t id = 0;
        int top = 0;
        int bottom = 0;
        double distance = 0;
    };
    std::vector<Column> columns(width);

    for (int column = 0; column < width; ++column) {
        double camera = 2.0 * (column + 0.5) / width - 1.0;
        double rayX = directionX + planeX * camera;
        double rayY = directionY + planeY * camera;
        Hit hit = castRay(floor, cameraX, cameraY, startCellX, startCellY, rayX, rayY);
        if (!hit.hit) continue;

        double wallHeight = projection / hit.distance;
        int top = static_cast<int>(std::floor(horizon - wallHeight / 2));
        int bottom = static_cast<int>(std::ceil(horizon + wallHeight / 2));
        Column& info = columns[column];
        info.hit = true;
        info.id = edgeKey(hit.edgeX, hit.edgeY) ^ (hit.vertical ? (1ull << 63) : 0);
        info.top = top;
        info.bottom = bottom;
        info.distance = hit.distance;

        double shade = distanceShade(hit.distance) * (hit.vertical ? 1.0 : 0.78);
        const TextureAsset& texture = hit.type == "door" ? doorTexture : wallTexture;
        SDL_Rect stripe{column, top, 1, bottom - top};
        if (texture.texture) {
            int sourceX = std::clamp(static_cast<int>(hit.wallX * texture.width), 0, texture.width - 1);
            SDL_Rect source{sourceX, 0, 1, texture.height};
            Uint8 mod = shadeChannel(255, shade);
            SDL_SetTextureColorMod(texture.texture, mod, mod, mod);
            SDL_RenderCopy(renderer, texture.texture, &source, &stripe);
        } else {
            if (hit.type == "door") SDL_SetRenderDrawColor(renderer, shadeChannel(230, shade), shadeChannel(190, shade), shadeChannel(70, shade), 255);
            else SDL_SetRenderDrawColor(renderer, shadeChannel(120, shade), shadeChannel(115, shade), shadeChannel(108, shade), 255);
            SDL_RenderFillRect(renderer, &stripe);
        }
    }
    if (wallTexture.texture) SDL_SetTextureColorMod(wallTexture.texture, 255, 255, 255);
    if (doorTexture.texture) SDL_SetTextureColorMod(doorTexture.texture, 255, 255, 255);

    // アウトライン: 壁の上下端と、辺(=1マス分の壁)の切れ目
    std::vector<SDL_Point> outline;
    outline.reserve(width * 4);
    for (int column = 0; column < width; ++column) {
        const Column& info = columns[column];
        if (!info.hit) continue;
        if (info.top >= 0) {
            outline.push_back({column, info.top});
            outline.push_back({column, info.top + 1});
        }
        if (info.bottom <= height) {
            outline.push_back({column, info.bottom - 1});
            outline.push_back({column, info.bottom - 2});
        }
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderDrawPoints(renderer, outline.data(), static_cast<int>(outline.size()));
    for (int column = 1; column < width; ++column) {
        const Column& left = columns[column - 1];
        const Column& right = columns[column];
        if (left.id == right.id && left.hit == right.hit) continue;
        // 手前側(背の高い方)の列に縦線を引く
        bool useLeft = left.hit && (!right.hit || left.distance < right.distance);
        const Column& nearer = useLeft ? left : right;
        if (!nearer.hit) continue;
        int x = useLeft ? column - 1 : column;
        SDL_Rect line{x, nearer.top, 2, nearer.bottom - nearer.top};
        if (useLeft) line.x -= 1;
        SDL_RenderFillRect(renderer, &line);
    }
}

void drawMapPreview(SDL_Renderer* renderer, const Floor& floor, const MapData& map,
                   double playerX, double playerY, double angle) {
    const int windowWidth = 420;
    const int windowHeight = 450;
    SDL_SetRenderDrawColor(renderer, 28, 29, 27, 255);
    SDL_RenderClear(renderer);

    const int margin = 30;
    const int titleHeight = 38;
    const int availableWidth = windowWidth - margin * 2;
    const int availableHeight = windowHeight - margin - titleHeight - margin;
    const int cellSize = std::max(8, std::min(availableWidth / floor.width,
                                               availableHeight / floor.height));
    const int gridWidth = cellSize * floor.width;
    const int gridHeight = cellSize * floor.height;
    const int originX = (windowWidth - gridWidth) / 2;
    const int originY = titleHeight + (availableHeight - gridHeight) / 2;

    SDL_Rect background{originX, originY, gridWidth, gridHeight};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, &background);

    auto setEdgeColor = [renderer](const std::string& type) {
        if (type == "door") SDL_SetRenderDrawColor(renderer, 230, 190, 70, 255);
        else SDL_SetRenderDrawColor(renderer, 220, 218, 210, 255);
    };
    for (const auto& edge : floor.hEdges) {
        int x = static_cast<int>(edge.first >> 32);
        int y = static_cast<int>(edge.first & 0xffffffffu);
        setEdgeColor(edge.second);
        SDL_Rect line{originX + x * cellSize, originY + y * cellSize - 1, cellSize + 1, 2};
        SDL_RenderFillRect(renderer, &line);
    }
    for (const auto& edge : floor.vEdges) {
        int x = static_cast<int>(edge.first >> 32);
        int y = static_cast<int>(edge.first & 0xffffffffu);
        setEdgeColor(edge.second);
        SDL_Rect line{originX + x * cellSize - 1, originY + y * cellSize, 2, cellSize + 1};
        SDL_RenderFillRect(renderer, &line);
    }

    for (const Stair& stair : map.stairs) {
        for (const StairEnd& end : stair.ends) {
            if (end.floorId != floor.id) continue;
            SDL_Rect marker{
                originX + end.x * cellSize + cellSize / 5,
                originY + end.y * cellSize + cellSize / 5,
                cellSize * 3 / 5,
                cellSize * 3 / 5
            };
            SDL_SetRenderDrawColor(renderer, 231, 168, 62, 255);
            SDL_RenderFillRect(renderer, &marker);
            std::string stairNumber = stair.id.compare(0, 6, "stair_") == 0 ? stair.id.substr(6) : stair.id;
            int stairTextX = marker.x + marker.w / 2;
            int stairTextY = marker.y + marker.h / 2;
            SDL_SetRenderDrawColor(renderer, 80, 48, 12, 255);
            int textWidth = static_cast<int>(stairNumber.size()) * 3;
            for (int digit = 0; digit < static_cast<int>(stairNumber.size()); ++digit) {
                SDL_RenderDrawPoint(renderer, stairTextX - textWidth / 2 + digit * 3, stairTextY);
                SDL_RenderDrawPoint(renderer, stairTextX - textWidth / 2 + digit * 3, stairTextY + 1);
            }
            SDL_RenderDrawLine(renderer, stairTextX, marker.y + 2, stairTextX, marker.y + marker.h - 2);
        }
    }

    const double directionX = std::cos(angle);
    const double directionY = std::sin(angle);
    double playerCenterX = originX + (playerX * cellSize);
    double playerCenterY = originY + (playerY * cellSize);
    int radius = std::max(4, cellSize / 3);
    SDL_Rect playerRect{
        static_cast<int>(playerCenterX) - radius,
        static_cast<int>(playerCenterY) - radius,
        radius * 2,
        radius * 2
    };
    SDL_SetRenderDrawColor(renderer, 91, 141, 239, 255);
    SDL_RenderFillRect(renderer, &playerRect);

    SDL_SetRenderDrawColor(renderer, 200, 220, 255, 255);
    int arrowX = static_cast<int>(playerCenterX + directionX * cellSize * 0.65);
    int arrowY = static_cast<int>(playerCenterY + directionY * cellSize * 0.65);
    for (int offset = -1; offset <= 1; ++offset) {
        SDL_RenderDrawLine(renderer, static_cast<int>(playerCenterX) - offset, static_cast<int>(playerCenterY) + offset,
                           arrowX - offset, arrowY + offset);
    }

    SDL_RenderPresent(renderer);
}

fs::path executableDirectory() {
    char* basePath = SDL_GetBasePath();
    fs::path result;
    if (basePath) {
        result = fs::path(basePath);
        SDL_free(basePath);
    }
    return result;
}

fs::path findMapPath(int argc, char* argv[], const fs::path& exeDir) {
    if (argc >= 2) return argv[1];
    const fs::path candidates[] = {
        exeDir / "map_editor/mymap00.json",
        exeDir / "../map_editor/mymap00.json",
        exeDir / "../../map_editor/mymap00.json",
        fs::path("map_editor/mymap00.json")
    };
    for (const fs::path& candidate : candidates) {
        if (fs::exists(candidate)) return candidate;
    }
    return candidates[0];
}

enum class Action { None, Forward, TurnLeft, TurnRight, TurnBack };

Action actionForKey(SDL_Scancode key) {
    switch (key) {
    case SDL_SCANCODE_UP: case SDL_SCANCODE_W: return Action::Forward;
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: return Action::TurnLeft;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: return Action::TurnRight;
    case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: return Action::TurnBack;
    default: return Action::None;
    }
}

Action heldAction() {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) return Action::Forward;
    if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A]) return Action::TurnLeft;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) return Action::TurnRight;
    return Action::None;  // 180° ターンは押しっぱなしで連続させない
}

// 論理位置(セル+向き)は入力時に即確定し、表示位置だけをアニメーションで追従させる
struct Motion {
    bool active = false;
    double progress = 0;
    double duration = 1;
    double fromX = 0, fromY = 0, toX = 0, toY = 0;
    double fromAngle = 0, toAngle = 0;
};

double smoothstep(double t) { return t * t * (3.0 - 2.0 * t); }

int main(int argc, char* argv[]) {
    fs::path exeDir = executableDirectory();
    try {
        MapData map = loadMap(findMapPath(argc, argv, exeDir));
        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
        if ((IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & (IMG_INIT_PNG | IMG_INIT_JPG)) == 0)
            throw std::runtime_error(IMG_GetError());

        SDL_Window* window = SDL_CreateWindow("maze3d", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                               kViewWidth, kViewHeight, SDL_WINDOW_SHOWN);
        if (!window) throw std::runtime_error(SDL_GetError());
        SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) throw std::runtime_error(SDL_GetError());

        SDL_Window* previewWindow = SDL_CreateWindow("map preview - current position", SDL_WINDOWPOS_UNDEFINED,
                                                     SDL_WINDOWPOS_UNDEFINED, 420, 450, SDL_WINDOW_SHOWN);
        if (!previewWindow) throw std::runtime_error(SDL_GetError());
        SDL_Renderer* previewRenderer = SDL_CreateRenderer(previewWindow, -1, SDL_RENDERER_ACCELERATED);
        if (!previewRenderer) {
            SDL_DestroyWindow(previewWindow);
            throw std::runtime_error(SDL_GetError());
        }
        SDL_RaiseWindow(window);

        std::string currentFloorId = map.spawn.floorId;
        const Floor* currentFloor = findFloor(map, currentFloorId);
        if (!currentFloor && !map.floors.empty()) {
            currentFloor = &map.floors.front();
            currentFloorId = currentFloor->id;
        }
        if (!currentFloor) throw std::runtime_error("Map contains no valid floor");

        int cellX = map.spawn.x;
        int cellY = map.spawn.y;
        int facing = 2;  // 西向きで開始
        double viewX = cellX + 0.5;
        double viewY = cellY + 0.5;
        double viewAngle = facing * kPi / 2;
        Motion motion;
        Action pendingAction = Action::None;

        fs::path base = exeDir / "assets/textures";
        if (!fs::exists(base)) base = fs::path("assets/textures");
        fs::path wallImage = findAsset(base / "wall", "stone_dark.png");
        fs::path doorImage = findAsset(base / "door", "door.png");
        TextureAsset wallTexture = loadTexture(renderer, wallImage);
        TextureAsset doorTexture = loadTexture(renderer, doorImage);

        auto startAction = [&](Action action) {
            if (action == Action::None) return;
            double angle = facing * kPi / 2;
            motion = {};
            motion.fromX = motion.toX = cellX + 0.5;
            motion.fromY = motion.toY = cellY + 0.5;
            motion.fromAngle = motion.toAngle = angle;
            if (action == Action::Forward) {
                int stepX = kDirX[facing];
                int stepY = kDirY[facing];
                if (moveBlocked(*currentFloor, cellX, cellY, stepX, stepY)) return;
                cellX += stepX;
                cellY += stepY;
                motion.toX = cellX + 0.5;
                motion.toY = cellY + 0.5;
                motion.duration = kMoveDuration;
            } else if (action == Action::TurnLeft) {
                facing = (facing + 3) % 4;
                motion.toAngle = angle - kPi / 2;
                motion.duration = kTurnDuration;
            } else if (action == Action::TurnRight) {
                facing = (facing + 1) % 4;
                motion.toAngle = angle + kPi / 2;
                motion.duration = kTurnDuration;
            } else if (action == Action::TurnBack) {
                facing = (facing + 2) % 4;
                motion.toAngle = angle + kPi;
                motion.duration = kTurnBackDuration;
            }
            motion.active = true;
        };

        bool running = true;
        Uint32 previousTime = SDL_GetTicks();
        while (running) {
            Uint32 currentTime = SDL_GetTicks();
            double frameTime = std::min((currentTime - previousTime) / 1000.0, 0.05);
            previousTime = currentTime;
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) running = false;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
                if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                    SDL_Scancode key = event.key.keysym.scancode;
                    if (key == SDL_SCANCODE_Q || key == SDL_SCANCODE_ESCAPE) running = false;
                    Action action = actionForKey(key);
                    if (action != Action::None) pendingAction = action;  // アニメ中の入力は1つだけ先行受付
                    if (key == SDL_SCANCODE_E && !motion.active) {
                        for (const Stair& stair : map.stairs) {
                            const StairEnd* destination = nullptr;
                            if (stair.ends[0].floorId == currentFloorId && stair.ends[0].x == cellX && stair.ends[0].y == cellY)
                                destination = &stair.ends[1];
                            else if (stair.ends[1].floorId == currentFloorId && stair.ends[1].x == cellX && stair.ends[1].y == cellY)
                                destination = &stair.ends[0];
                            if (!destination) continue;
                            const Floor* target = findFloor(map, destination->floorId);
                            if (!target) break;
                            currentFloorId = destination->floorId;
                            currentFloor = target;
                            cellX = destination->x;
                            cellY = destination->y;
                            viewX = cellX + 0.5;
                            viewY = cellY + 0.5;
                            break;
                        }
                    }
                }
            }

            if (motion.active) {
                motion.progress = std::min(1.0, motion.progress + frameTime / motion.duration);
                double eased = smoothstep(motion.progress);
                viewX = motion.fromX + (motion.toX - motion.fromX) * eased;
                viewY = motion.fromY + (motion.toY - motion.fromY) * eased;
                viewAngle = motion.fromAngle + (motion.toAngle - motion.fromAngle) * eased;
                if (motion.progress >= 1.0) motion.active = false;
            }
            if (!motion.active) {
                viewX = cellX + 0.5;
                viewY = cellY + 0.5;
                viewAngle = facing * kPi / 2;
                Action next = pendingAction != Action::None ? pendingAction : heldAction();
                pendingAction = Action::None;
                startAction(next);
            }

            drawPseudo3D(renderer, *currentFloor, wallTexture, doorTexture, viewX, viewY, viewAngle);
            SDL_RenderPresent(renderer);
            drawMapPreview(previewRenderer, *currentFloor, map, viewX, viewY, viewAngle);
        }

        if (wallTexture.texture) SDL_DestroyTexture(wallTexture.texture);
        if (doorTexture.texture) SDL_DestroyTexture(doorTexture.texture);
        SDL_DestroyRenderer(previewRenderer);
        SDL_DestroyWindow(previewWindow);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        IMG_Quit();
        SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "maze3d: " << error.what() << std::endl;
        if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "maze3d startup error",
                                     error.what(), nullptr);
        }
        return 1;
    }
}
