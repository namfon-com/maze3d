#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

struct StairEnd { std::string floorId; int x = 0; int y = 0; };
struct Stair { std::string id; StairEnd ends[2]; };
// セルに置くイベント。type: "message"（メッセージ表示）/ "treasure"（宝箱: 1回だけ gold / item を入手）
// / "recruit"（人間に出会う: character の人物を仲間に誘える）/ "warp"（踏むと warpTo へ飛ばされる）
struct MapEvent {
    std::string type;
    std::string text;
    bool once = false;
    int gold = 0;
    std::string item;
    std::string character;
    std::string warpFloor;
    int warpX = 0;
    int warpY = 0;
    int warpFacing = -1;  // 到着時の向き。-1 なら向きはそのまま
};
struct Floor {
    std::string id;
    int width = 0;
    int height = 0;
    std::unordered_map<std::uint64_t, std::string> hEdges;
    std::unordered_map<std::uint64_t, std::string> vEdges;
    std::unordered_map<std::uint64_t, MapEvent> events;  // key = edgeKey(セルx, セルy)
    // 看板: そのセルで指定の向きを向いて立ち止まっている間だけ表示する（操作は止めない）
    std::unordered_map<std::uint64_t, std::string> signs[4];  // [facing][edgeKey(セルx, セルy)] = 文字
};
struct MapData {
    int version = 1;
    std::vector<Floor> floors;
    std::vector<Stair> stairs;
    StairEnd spawn;
    int spawnFacing = 3;  // facing 番号 (0=東 1=南 2=西 3=北)。省略時は北
};

std::uint64_t edgeKey(int x, int y) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32)
        | static_cast<std::uint32_t>(y);
}

const Floor* findFloor(const MapData& map, const std::string& id) {
    for (const Floor& floor : map.floors) if (floor.id == id) return &floor;
    return nullptr;
}

// "E"/"S"/"W"/"N" → facing 番号 (0=東 1=南 2=西 3=北)。不正なら -1
int facingIndex(const std::string& facing) {
    if (facing == "E") return 0;
    if (facing == "S") return 1;
    if (facing == "W") return 2;
    if (facing == "N") return 3;
    return -1;
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
        for (const auto& evj : fj.value("events", json::array())) {
            MapEvent event;
            event.type = evj.at("type").get<std::string>();
            if (event.type != "message" && event.type != "treasure" && event.type != "recruit" && event.type != "warp")
                throw std::runtime_error("Unknown event type: " + event.type);
            event.text = evj.value("text", "");
            event.once = evj.value("once", false);
            event.gold = evj.value("gold", 0);
            event.item = evj.value("item", "");
            event.character = evj.value("character", "");
            if (event.type == "warp") {
                const json& to = evj.at("to");
                event.warpFloor = to.at("floorId").get<std::string>();
                event.warpX = to.at("locate").at(0).get<int>();
                event.warpY = to.at("locate").at(1).get<int>();
                if (to.contains("facing")) {
                    event.warpFacing = facingIndex(to["facing"].get<std::string>());
                    if (event.warpFacing < 0) throw std::runtime_error("warp facing must be one of N/E/S/W");
                }
            }
            int x = evj.at("locate").at(0).get<int>();
            int y = evj.at("locate").at(1).get<int>();
            floor.events[edgeKey(x, y)] = event;
        }
        for (const auto& sj : fj.value("signs", json::array())) {
            int facing = facingIndex(sj.at("facing").get<std::string>());
            if (facing < 0) throw std::runtime_error("sign.facing must be one of N/E/S/W");
            int x = sj.at("locate").at(0).get<int>();
            int y = sj.at("locate").at(1).get<int>();
            floor.signs[facing][edgeKey(x, y)] = sj.at("text").get<std::string>();
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
        map.spawnFacing = facingIndex(document["spawn"].value("facing", "N"));
        if (map.spawnFacing < 0) throw std::runtime_error("spawn.facing must be one of N/E/S/W");
    } else if (!map.floors.empty()) {
        map.spawn.floorId = map.floors.front().id;
    }
    return map;
}

// キャラクター定義（assets/data/characters.json）。主人公も仲間候補も同じ形式
struct Character {
    std::string id;
    std::string name;
    std::string portrait;  // assets/portraits/ 内のファイル名（64x64 推奨）
    int level = 1;
    int exp = 0;
    int hp = 1;
    int maxHp = 1;
    int str = 1;  // ちから: 攻撃力
    int vit = 1;  // たいりょく: 防御・最大HP
    int agi = 1;  // すばやさ: 行動順・回避・逃走
    int gold = 0; // 所持金は各人が持つ（Black Onyx 風）
};

struct CharacterDatabase {
    std::string heroId;
    std::unordered_map<std::string, Character> characters;
};

CharacterDatabase loadCharacters(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open: " + path.string());
    json document;
    in >> document;
    CharacterDatabase database;
    database.heroId = document.at("hero").get<std::string>();
    for (const auto& cj : document.at("characters")) {
        Character character;
        character.id = cj.at("id").get<std::string>();
        character.name = cj.at("name").get<std::string>();
        character.portrait = cj.value("portrait", "");
        character.level = cj.value("level", 1);
        character.exp = cj.value("exp", 0);
        character.maxHp = cj.at("hp").get<int>();
        character.hp = character.maxHp;
        character.str = cj.at("str").get<int>();
        character.vit = cj.at("vit").get<int>();
        character.agi = cj.at("agi").get<int>();
        character.gold = cj.value("gold", 0);
        database.characters[character.id] = character;
    }
    if (!database.characters.count(database.heroId))
        throw std::runtime_error("characters.json: hero '" + database.heroId + "' is not defined");
    return database;
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

std::string eventId(const std::string& floorId, int x, int y) {
    return floorId + ":" + std::to_string(x) + "," + std::to_string(y);
}

void drawMapPreview(SDL_Renderer* renderer, const Floor& floor, const MapData& map,
                   const std::unordered_set<std::string>& doneEvents,
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

    // 壁: 細線。ドア: 黄色の細線 + 中央に太いバー（扉板）
    const int doorLength = std::max(4, cellSize * 56 / 100);
    const int doorThickness = std::max(4, cellSize / 6);
    auto drawEdge = [&](std::uint64_t key, const std::string& type, bool horizontal) {
        int x = static_cast<int>(key >> 32);
        int y = static_cast<int>(key & 0xffffffffu);
        int px = originX + x * cellSize;
        int py = originY + y * cellSize;
        if (type == "door") SDL_SetRenderDrawColor(renderer, 242, 194, 48, 255);
        else SDL_SetRenderDrawColor(renderer, 220, 218, 210, 255);
        SDL_Rect line = horizontal ? SDL_Rect{px, py - 1, cellSize + 1, 2}
                                   : SDL_Rect{px - 1, py, 2, cellSize + 1};
        SDL_RenderFillRect(renderer, &line);
        if (type != "door") return;
        SDL_Rect bar = horizontal
            ? SDL_Rect{px + (cellSize - doorLength) / 2, py - doorThickness / 2, doorLength, doorThickness}
            : SDL_Rect{px - doorThickness / 2, py + (cellSize - doorLength) / 2, doorThickness, doorLength};
        SDL_RenderFillRect(renderer, &bar);
    };
    for (const auto& edge : floor.hEdges) drawEdge(edge.first, edge.second, true);
    for (const auto& edge : floor.vEdges) drawEdge(edge.first, edge.second, false);

    for (const auto& [key, event] : floor.events) {
        int x = static_cast<int>(key >> 32);
        int y = static_cast<int>(key & 0xffffffffu);
        int size = std::max(4, cellSize / 3);
        SDL_Rect marker{originX + x * cellSize + (cellSize - size) / 2,
                        originY + y * cellSize + (cellSize - size) / 2, size, size};
        bool done = doneEvents.count(eventId(floor.id, x, y)) > 0;
        if (event.type == "treasure") {
            if (done) SDL_SetRenderDrawColor(renderer, 90, 70, 50, 255);
            else SDL_SetRenderDrawColor(renderer, 201, 139, 60, 255);
        } else if (event.type == "recruit") {
            if (done) continue;  // 仲間になった人物はもういない
            SDL_SetRenderDrawColor(renderer, 110, 210, 120, 255);
        } else if (event.type == "warp") {
            SDL_SetRenderDrawColor(renderer, 170, 120, 240, 255);
        } else {
            if (done) SDL_SetRenderDrawColor(renderer, 60, 80, 110, 255);
            else SDL_SetRenderDrawColor(renderer, 120, 180, 255, 255);
        }
        SDL_RenderFillRect(renderer, &marker);
    }

    // 看板: 見る向きの辺の内側に小さな札
    SDL_SetRenderDrawColor(renderer, 255, 230, 160, 255);
    for (int facing = 0; facing < 4; ++facing) {
        for (const auto& [key, text] : floor.signs[facing]) {
            int x = static_cast<int>(key >> 32);
            int y = static_cast<int>(key & 0xffffffffu);
            int size = std::max(3, cellSize / 6);
            int centerX = originX + x * cellSize + cellSize / 2 + kDirX[facing] * (cellSize / 2 - size);
            int centerY = originY + y * cellSize + cellSize / 2 + kDirY[facing] * (cellSize / 2 - size);
            SDL_Rect tag{centerX - size / 2, centerY - size / 2, size, size};
            SDL_RenderFillRect(renderer, &tag);
        }
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
    const fs::path mapFile = fs::path("map_editor") / "utsuro.json";
    const fs::path candidates[] = {
        exeDir / mapFile,
        exeDir / ".." / mapFile,
        exeDir / ".." / ".." / mapFile,
        mapFile
    };
    for (const fs::path& candidate : candidates) {
        if (fs::exists(candidate)) return candidate;
    }
    return candidates[0];
}

// 画面下部のメッセージウィンドウ。キューの先頭を表示し、決定キーで次へ進む。
// ウィンドウに収まらない文章は kMessageLines 行ずつ送り、送るたびに上へスクロールさせる
constexpr int kFontPixel = 12;  // PixelMplus12 の設計サイズ。整数倍で拡大してドットをくっきり保つ
constexpr int kTextScale = 2;
constexpr int kMessageLines = 4;
constexpr double kMessageScrollDuration = 0.18;  // 1ページ分のスクロール時間（秒）

struct Message {
    std::string text;
    std::string portrait;                 // 顔を出すキャラクターID（空なら顔なし）
    std::function<void(bool)> onChoice;   // 設定時は最後のページで「はい／いいえ」を選ばせる
};

struct MessageWindow {
    std::deque<Message> queue;
    SDL_Texture* texture = nullptr;  // queue.front() の描画キャッシュ
    int textWidth = 0;
    int textHeight = 0;
    int lineSkip = kFontPixel;
    int topLine = 0;        // 表示中ページの先頭行（スクロール目標）
    double scrollY = 0;     // 現在のスクロール位置（テクスチャ上のピクセル）
    Uint32 lastTicks = 0;

    bool open() const { return !queue.empty(); }
    int totalLines() const { return lineSkip > 0 ? (textHeight + lineSkip - 1) / lineSkip : 0; }
    bool hasMorePages() const { return topLine + kMessageLines < totalLines(); }
    bool choiceYes = true;

    void push(const std::string& text) { queue.push_back({text, "", nullptr}); }
    void push(Message message) { queue.push_back(std::move(message)); }
    bool choosing() const { return open() && queue.front().onChoice && texture && !hasMorePages(); }
    void advance() {
        if (queue.empty()) return;
        if (texture && hasMorePages()) {
            topLine += kMessageLines;
            return;
        }
        // コールバックが新しいメッセージを積めるよう、先に取り出してから呼ぶ
        std::function<void(bool)> callback = queue.front().onChoice;
        bool answer = choiceYes;
        queue.pop_front();
        release();
        choiceYes = true;
        if (callback) callback(answer);
    }
    void release() {
        if (texture) SDL_DestroyTexture(texture);
        texture = nullptr;
        topLine = 0;
        scrollY = 0;
    }
};

// 日本語向けの1文字単位の折り返し。句読点・閉じ括弧は行頭に来ないよう前の行にぶら下げる
std::string wrapText(TTF_Font* font, const std::string& text, int maxWidth) {
    static const std::string noLineStart[] = {"、", "。", "，", "．", "」", "』", "）", "】", "！", "？", "…", "ー", "っ", "ッ", "ゃ", "ゅ", "ょ"};
    std::string result;
    std::string line;
    for (std::size_t i = 0; i < text.size();) {
        unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        std::string glyph = text.substr(i, length);
        i += length;
        if (glyph == "\n") {
            result += line + "\n";
            line.clear();
            continue;
        }
        int width = 0;
        TTF_SizeUTF8(font, (line + glyph).c_str(), &width, nullptr);
        bool hanging = std::find(std::begin(noLineStart), std::end(noLineStart), glyph) != std::end(noLineStart);
        if (width > maxWidth && !line.empty() && !hanging) {
            result += line + "\n";
            line.clear();
            if (glyph == " ") continue;  // 行頭の空白は捨てる
        }
        line += glyph;
    }
    return result + line;
}

SDL_Texture* renderText(SDL_Renderer* renderer, TTF_Font* font, const std::string& text,
                        int wrapWidth, int& width, int& height) {
    SDL_Color white{235, 235, 225, 255};
    std::string wrapped = wrapText(font, text, wrapWidth);
    // wrapLength 0 = 改行文字でのみ改行（折り返し位置は wrapText で決めている）
    SDL_Surface* surface = TTF_RenderUTF8_Solid_Wrapped(font, wrapped.empty() ? " " : wrapped.c_str(), white, 0);
    if (!surface) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    width = surface->w;
    height = surface->h;
    SDL_FreeSurface(surface);
    return texture;
}

// 1行テキストの描画キャッシュ（パーティ欄や選択肢など、毎フレーム描く短い文字列用）
struct TextCache {
    struct Entry {
        SDL_Texture* texture = nullptr;
        int width = 0;
        int height = 0;
    };
    std::unordered_map<std::string, Entry> entries;

    const Entry& get(SDL_Renderer* renderer, TTF_Font* font, const std::string& text) {
        auto found = entries.find(text);
        if (found != entries.end()) return found->second;
        if (entries.size() > 512) clear();
        Entry entry;
        entry.texture = renderText(renderer, font, text, 1 << 20, entry.width, entry.height);
        return entries.emplace(text, entry).first->second;
    }
    void clear() {
        for (auto& [text, entry] : entries) if (entry.texture) SDL_DestroyTexture(entry.texture);
        entries.clear();
    }
};

void drawText(SDL_Renderer* renderer, TTF_Font* font, TextCache& cache, const std::string& text,
              int x, int y, SDL_Color color = {235, 235, 225, 255}) {
    const TextCache::Entry& entry = cache.get(renderer, font, text);
    if (!entry.texture) return;
    SDL_SetTextureColorMod(entry.texture, color.r, color.g, color.b);
    SDL_Rect destination{x, y, entry.width * kTextScale, entry.height * kTextScale};
    SDL_RenderCopy(renderer, entry.texture, nullptr, &destination);
}

void drawFrame(SDL_Renderer* renderer, const SDL_Rect& frame) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 225);
    SDL_RenderFillRect(renderer, &frame);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    // 二重枠（外: 太い白線、内: 細線）
    SDL_SetRenderDrawColor(renderer, 235, 235, 225, 255);
    for (int i = 0; i < 3; ++i) {
        SDL_Rect border{frame.x + i, frame.y + i, frame.w - i * 2, frame.h - i * 2};
        SDL_RenderDrawRect(renderer, &border);
    }
    SDL_Rect inner{frame.x + 7, frame.y + 7, frame.w - 14, frame.h - 14};
    SDL_RenderDrawRect(renderer, &inner);
}

// 看板: 3D 画面の上部中央に、枠付きのプレートで文字を出す。alpha (0..1) でフェードイン
void drawSign(SDL_Renderer* renderer, TTF_Font* font, TextCache& cache, const std::string& text, double alpha) {
    const TextCache::Entry& entry = cache.get(renderer, font, text);
    if (!entry.texture) return;
    Uint8 a = static_cast<Uint8>(std::clamp(alpha, 0.0, 1.0) * 255);
    const int textWidth = entry.width * kTextScale;
    const int textHeight = entry.height * kTextScale;
    SDL_Rect plate{(kViewWidth - textWidth) / 2 - 24, 28, textWidth + 48, textHeight + 24};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 20, 14, 6, static_cast<Uint8>(a * 0.85));
    SDL_RenderFillRect(renderer, &plate);
    SDL_SetRenderDrawColor(renderer, 242, 194, 48, a);
    for (int i = 0; i < 2; ++i) {
        SDL_Rect border{plate.x + i, plate.y + i, plate.w - i * 2, plate.h - i * 2};
        SDL_RenderDrawRect(renderer, &border);
    }
    SDL_Rect inner{plate.x + 5, plate.y + 5, plate.w - 10, plate.h - 10};
    SDL_RenderDrawRect(renderer, &inner);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetTextureColorMod(entry.texture, 255, 230, 160);
    SDL_SetTextureAlphaMod(entry.texture, a);
    SDL_Rect destination{plate.x + 24, plate.y + 12, textWidth, textHeight};
    SDL_RenderCopy(renderer, entry.texture, nullptr, &destination);
    SDL_SetTextureAlphaMod(entry.texture, 255);
}

// 顔グラフィック。assets/portraits/ の画像を使い、無ければ人物シルエットを描く
constexpr int kPortraitSize = 64;

struct PortraitCache {
    fs::path directory;
    std::unordered_map<std::string, TextureAsset> textures;

    void draw(SDL_Renderer* renderer, const Character& character, const SDL_Rect& destination) {
        auto found = textures.find(character.id);
        if (found == textures.end()) {
            TextureAsset asset;
            if (!character.portrait.empty()) asset = loadTexture(renderer, directory / character.portrait);
            found = textures.emplace(character.id, asset).first;
        }
        if (found->second.texture) {
            SDL_RenderCopy(renderer, found->second.texture, nullptr, &destination);
            return;
        }
        // 仮の顔: ID から決めた色の背景に、頭と肩のシルエット
        std::size_t hash = std::hash<std::string>{}(character.id);
        SDL_SetRenderDrawColor(renderer, 40 + hash % 60, 40 + (hash >> 8) % 60, 50 + (hash >> 16) % 70, 255);
        SDL_RenderFillRect(renderer, &destination);
        SDL_SetRenderDrawColor(renderer, 12, 12, 16, 255);
        double unit = destination.w / 64.0;
        int centerX = destination.x + destination.w / 2;
        for (int row = 0; row < destination.h; ++row) {
            double y = row / unit;
            double half = 0;
            if (y >= 12 && y <= 36) half = std::sqrt(std::max(0.0, 144.0 - (y - 24) * (y - 24)));  // 頭
            if (y >= 40) half = std::min(28.0, 14.0 + (y - 40) * 1.2);                             // 肩
            if (half <= 0) continue;
            SDL_RenderDrawLine(renderer, centerX - static_cast<int>(half * unit), destination.y + row,
                               centerX + static_cast<int>(half * unit), destination.y + row);
        }
    }
    void release() {
        for (auto& [id, asset] : textures) if (asset.texture) SDL_DestroyTexture(asset.texture);
        textures.clear();
    }
};

void drawMessageWindow(SDL_Renderer* renderer, TTF_Font* font, TextCache& cache, PortraitCache& portraits,
                       const std::unordered_map<std::string, Character>& characters,
                       MessageWindow& window, Uint32 ticks) {
    if (!window.open()) return;
    const int padding = 22;
    window.lineSkip = std::max(1, TTF_FontLineSkip(font));
    const Message& message = window.queue.front();
    auto speaker = characters.find(message.portrait);
    const bool hasPortrait = speaker != characters.end();
    const int portraitSize = kPortraitSize * kTextScale;
    const int textAreaHeight = kMessageLines * window.lineSkip * kTextScale;
    const int frameHeight = std::max(textAreaHeight, hasPortrait ? portraitSize : 0) + padding * 2;
    const SDL_Rect frame{24, kViewHeight - 24 - frameHeight, kViewWidth - 48, frameHeight};
    const int textX = frame.x + padding + (hasPortrait ? portraitSize + 20 : 0);
    const int textWidth = frame.x + frame.w - padding - textX;
    if (!window.texture) {
        int wrap = textWidth / kTextScale - kFontPixel;  // ぶら下げ1文字分の余白を残す
        window.texture = renderText(renderer, font, message.text, wrap, window.textWidth, window.textHeight);
    }
    // スクロール位置を目標ページへ近づける
    double elapsed = window.lastTicks ? (ticks - window.lastTicks) / 1000.0 : 0.0;
    window.lastTicks = ticks;
    double targetY = window.topLine * window.lineSkip;
    double step = kMessageLines * window.lineSkip / kMessageScrollDuration * std::min(elapsed, 0.05);
    window.scrollY = window.scrollY < targetY ? std::min(targetY, window.scrollY + step) : targetY;
    drawFrame(renderer, frame);
    if (hasPortrait) {
        SDL_Rect face{frame.x + padding, frame.y + padding, portraitSize, portraitSize};
        portraits.draw(renderer, speaker->second, face);
    }

    if (window.texture) {
        SDL_Rect clip{textX, frame.y + padding, textWidth, textAreaHeight};
        SDL_RenderSetClipRect(renderer, &clip);
        SDL_Rect destination{textX,
                             frame.y + padding - static_cast<int>(window.scrollY) * kTextScale,
                             window.textWidth * kTextScale, window.textHeight * kTextScale};
        SDL_RenderCopy(renderer, window.texture, nullptr, &destination);
        SDL_RenderSetClipRect(renderer, nullptr);
    }
    bool scrolling = window.scrollY < window.topLine * window.lineSkip;
    // はい／いいえ の選択肢（メッセージ枠の右上に小窓を出す）
    if (window.choosing() && !scrolling) {
        const int lineHeight = window.lineSkip * kTextScale;
        SDL_Rect box{frame.x + frame.w - 200, frame.y - lineHeight * 2 - 44, 200, lineHeight * 2 + 40};
        drawFrame(renderer, box);
        const char* labels[] = {"はい", "いいえ"};
        for (int i = 0; i < 2; ++i) {
            int y = box.y + 20 + i * lineHeight;
            bool selected = (i == 0) == window.choiceYes;
            if (selected) {  // カーソル（右向き三角）
                SDL_SetRenderDrawColor(renderer, 255, 230, 120, 255);
                for (int col = 0; col < 9; ++col)
                    SDL_RenderDrawLine(renderer, box.x + 22 + col, y + 4 + col, box.x + 22 + col, y + 20 - col);
            }
            drawText(renderer, font, cache, labels[i], box.x + 22 + kFontPixel * kTextScale + 6, y,
                     selected ? SDL_Color{255, 230, 120, 255} : SDL_Color{235, 235, 225, 255});
        }
        return;  // 選択中は送りカーソルを出さない
    }
    // 送り待ちの点滅カーソル（スクロール中は出さない）
    if (!scrolling && (ticks / 400) % 2 == 0) {
        int cx = frame.x + frame.w - padding - 8;
        int cy = frame.y + frame.h - padding - 6;
        for (int row = 0; row < 7; ++row) {
            SDL_RenderDrawLine(renderer, cx - 7 + row, cy + row, cx + 7 - row, cy + row);
        }
    }
}

// 3D画面の右に並べるパーティ欄（最大 kMaxParty 人）
constexpr int kPanelWidth = 240;
constexpr int kMaxParty = 5;

void drawPartyPanel(SDL_Renderer* renderer, TTF_Font* font, TextCache& cache, PortraitCache& portraits,
                    const std::vector<Character>& party) {
    SDL_Rect panel{kViewWidth, 0, kPanelWidth, kViewHeight};
    SDL_SetRenderDrawColor(renderer, 10, 10, 14, 255);
    SDL_RenderFillRect(renderer, &panel);
    const int rowHeight = kViewHeight / kMaxParty;
    for (int i = 0; i < kMaxParty; ++i) {
        SDL_Rect row{panel.x + 4, i * rowHeight + 4, panel.w - 8, rowHeight - 8};
        SDL_SetRenderDrawColor(renderer, 90, 90, 84, 255);
        SDL_RenderDrawRect(renderer, &row);
        if (i >= static_cast<int>(party.size())) {
            drawText(renderer, font, cache, "---", row.x + row.w / 2 - kFontPixel, row.y + row.h / 2 - kFontPixel,
                     SDL_Color{80, 80, 76, 255});
            continue;
        }
        const Character& member = party[i];
        SDL_Rect face{row.x + 8, row.y + 8, kPortraitSize, kPortraitSize};
        portraits.draw(renderer, member, face);
        SDL_SetRenderDrawColor(renderer, 160, 160, 150, 255);
        SDL_RenderDrawRect(renderer, &face);

        // HPバー（顔の下）
        double ratio = member.maxHp > 0 ? std::clamp(static_cast<double>(member.hp) / member.maxHp, 0.0, 1.0) : 0.0;
        SDL_Color hpColor = ratio > 0.5 ? SDL_Color{120, 220, 120, 255}
                          : ratio > 0.25 ? SDL_Color{240, 210, 80, 255} : SDL_Color{240, 90, 80, 255};
        SDL_Rect barBack{face.x, face.y + face.h + 8, face.w, 8};
        SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
        SDL_RenderFillRect(renderer, &barBack);
        SDL_Rect bar{barBack.x, barBack.y, static_cast<int>(barBack.w * ratio), barBack.h};
        SDL_SetRenderDrawColor(renderer, hpColor.r, hpColor.g, hpColor.b, 255);
        SDL_RenderFillRect(renderer, &bar);

        const int textX = face.x + face.w + 10;
        const int line = kFontPixel * kTextScale + 2;
        drawText(renderer, font, cache, member.name, textX, row.y + 6);
        drawText(renderer, font, cache, "LV " + std::to_string(member.level), textX, row.y + 6 + line,
                 SDL_Color{180, 180, 170, 255});
        drawText(renderer, font, cache, "HP " + std::to_string(member.hp) + "/" + std::to_string(member.maxHp),
                 textX, row.y + 6 + line * 2, hpColor);
        drawText(renderer, font, cache, "G  " + std::to_string(member.gold), textX, row.y + 6 + line * 3,
                 SDL_Color{240, 200, 90, 255});
    }
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
    bool moved = false;  // 1マス移動（到着時にイベント判定する）
};

// パーティ・所持品とイベント消化状況
struct GameState {
    std::vector<Character> party;                // 先頭がリーダー（主人公）
    std::vector<std::string> items;              // アイテムは当面パーティ共有
    std::unordered_set<std::string> doneEvents;  // eventId(): once メッセージ表示済み / 宝箱開封済み / 勧誘済み
};

fs::path findAssetPath(const fs::path& exeDir, const fs::path& relative) {
    fs::path candidate = exeDir / "assets" / relative;
    return fs::exists(candidate) ? candidate : fs::path("assets") / relative;
}

double smoothstep(double t) { return t * t * (3.0 - 2.0 * t); }

int main(int argc, char* argv[]) {
    fs::path exeDir = executableDirectory();
    try {
        MapData map = loadMap(findMapPath(argc, argv, exeDir));
        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
        if ((IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & (IMG_INIT_PNG | IMG_INIT_JPG)) == 0)
            throw std::runtime_error(IMG_GetError());
        if (TTF_Init() != 0) throw std::runtime_error(TTF_GetError());

        // メイン画面とマッププレビューを横に並べ、2枚まとめて画面中央に配置する
        const int previewWidth = 420;
        const int previewHeight = 450;
        const int windowGap = 12;
        SDL_Rect display{0, 0, 1280, 720};
        SDL_GetDisplayUsableBounds(0, &display);
        const int mainWidth = kViewWidth + kPanelWidth;
        int mainX = display.x + std::max(0, (display.w - (mainWidth + windowGap + previewWidth)) / 2);
        int mainY = display.y + std::max(0, (display.h - kViewHeight) / 2);
        SDL_Window* window = SDL_CreateWindow("maze3d", mainX, mainY, mainWidth, kViewHeight, SDL_WINDOW_SHOWN);
        if (!window) throw std::runtime_error(SDL_GetError());
        SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) throw std::runtime_error(SDL_GetError());

        SDL_Window* previewWindow = SDL_CreateWindow("map preview - current position", mainX + mainWidth + windowGap, mainY,
                                                     previewWidth, previewHeight, SDL_WINDOW_SHOWN);
        if (!previewWindow) throw std::runtime_error(SDL_GetError());
        SDL_Renderer* previewRenderer = SDL_CreateRenderer(previewWindow, -1, SDL_RENDERER_ACCELERATED);
        if (!previewRenderer) {
            SDL_DestroyWindow(previewWindow);
            throw std::runtime_error(SDL_GetError());
        }
        // 起動時に両ウィンドウを最前面へ。キーボード入力を受けるメイン画面を最後に前面化する
        SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
        SDL_RaiseWindow(previewWindow);
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
        int facing = map.spawnFacing;
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

        fs::path fontPath = exeDir / "assets/fonts/PixelMplus12-Regular.ttf";
        if (!fs::exists(fontPath)) fontPath = fs::path("assets/fonts/PixelMplus12-Regular.ttf");
        TTF_Font* font = TTF_OpenFont(fontPath.string().c_str(), kFontPixel);
        if (!font) throw std::runtime_error(std::string("Cannot load font: ") + TTF_GetError());

        CharacterDatabase database = loadCharacters(findAssetPath(exeDir, "data/characters.json"));
        for (const Floor& floor : map.floors) {
            for (const auto& [key, event] : floor.events) {
                if (event.type == "recruit" && !database.characters.count(event.character))
                    throw std::runtime_error("Recruit event refers to unknown character: " + event.character);
                if (event.type == "warp") {
                    const Floor* target = findFloor(map, event.warpFloor);
                    if (!target || event.warpX < 0 || event.warpY < 0
                        || event.warpX >= target->width || event.warpY >= target->height)
                        throw std::runtime_error("Warp destination is outside the map: " + event.warpFloor);
                }
            }
        }
        PortraitCache portraits;
        portraits.directory = findAssetPath(exeDir, "portraits");
        TextCache textCache;

        GameState state;
        state.party.push_back(database.characters.at(database.heroId));
        MessageWindow messages;

        // ワープ: 文章があれば読み終えてから飛ぶ。着地点のイベントは起こさない（ワープの往復ループ防止）
        const MapEvent* pendingWarp = nullptr;
        Uint32 warpFlashStart = 0;
        constexpr double kWarpFlashMs = 400.0;
        auto warpTo = [&](const MapEvent& warp) {
            currentFloorId = warp.warpFloor;
            currentFloor = findFloor(map, currentFloorId);
            cellX = warp.warpX;
            cellY = warp.warpY;
            if (warp.warpFacing >= 0) facing = warp.warpFacing;
            motion = {};
            pendingAction = Action::None;
            viewX = cellX + 0.5;
            viewY = cellY + 0.5;
            viewAngle = facing * kPi / 2;
            warpFlashStart = SDL_GetTicks();
        };

        auto triggerEvent = [&]() {
            auto found = currentFloor->events.find(edgeKey(cellX, cellY));
            if (found == currentFloor->events.end()) return;
            const MapEvent& event = found->second;
            std::string id = eventId(currentFloorId, cellX, cellY);
            bool done = state.doneEvents.count(id) > 0;
            if (event.type == "warp") {
                if (!event.text.empty()) messages.push(event.text);
                pendingWarp = &event;
            } else if (event.type == "message") {
                if (event.once && done) return;
                messages.push(event.text);
                if (event.once) state.doneEvents.insert(id);
            } else if (event.type == "treasure") {
                if (done) {
                    messages.push("宝箱は空っぽだ。");
                    return;
                }
                std::string text = event.text.empty() ? "" : event.text + "\n";
                text += "宝箱を開けた！";
                if (event.gold > 0) {
                    Character& leader = state.party.front();  // 拾ったお金はリーダーの所持金になる
                    leader.gold += event.gold;
                    text += "\n" + leader.name + " は " + std::to_string(event.gold) + " ゴールドを手に入れた。";
                }
                if (!event.item.empty()) {
                    state.items.push_back(event.item);
                    text += "\n" + event.item + " を手に入れた。";
                }
                messages.push(text);
                state.doneEvents.insert(id);
            } else if (event.type == "recruit") {
                if (done) return;
                const Character& candidate = database.characters.at(event.character);
                bool alreadyMember = std::any_of(state.party.begin(), state.party.end(),
                                                 [&](const Character& member) { return member.id == candidate.id; });
                if (alreadyMember) return;
                std::string text = event.text.empty()
                    ? candidate.name + " がいる。\n「冒険の仲間を探しているのか？」"
                    : event.text;
                text += "\n" + candidate.name + " を仲間に誘いますか？";
                messages.push(Message{text, candidate.id, [&state, &messages, candidate, id](bool yes) {
                    if (!yes) {
                        messages.push(Message{"「そうか。気が変わったら声をかけてくれ。」", candidate.id, nullptr});
                        return;
                    }
                    if (static_cast<int>(state.party.size()) >= kMaxParty) {
                        messages.push("これ以上は仲間にできない。");
                        return;
                    }
                    state.party.push_back(candidate);
                    state.doneEvents.insert(id);
                    messages.push(candidate.name + " が仲間に加わった！");
                }});
            }
        };

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
                motion.moved = true;
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

        const std::string* shownSign = nullptr;
        Uint32 signStart = 0;
        constexpr double kSignFadeMs = 80.0;

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
                    if (messages.open()) {
                        // メッセージ表示中は送り操作（選択肢があれば上下で選択、X/Backspace で「いいえ」）のみ受け付ける
                        bool confirm = key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER
                                    || key == SDL_SCANCODE_SPACE || key == SDL_SCANCODE_Z;
                        if (messages.choosing()) {
                            if (actionForKey(key) != Action::None) messages.choiceYes = !messages.choiceYes;
                            if (key == SDL_SCANCODE_X || key == SDL_SCANCODE_BACKSPACE) {
                                messages.choiceYes = false;
                                confirm = true;
                            }
                        }
                        if (confirm) messages.advance();
                        continue;
                    }
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
                if (motion.progress >= 1.0) {
                    motion.active = false;
                    if (motion.moved) {
                        triggerEvent();
                        if (messages.open()) pendingAction = Action::None;  // 先行入力でイベントを素通りしない
                    }
                }
            }
            if (pendingWarp && !motion.active && !messages.open()) {
                warpTo(*pendingWarp);
                pendingWarp = nullptr;
            }
            if (!motion.active && !messages.open()) {
                viewX = cellX + 0.5;
                viewY = cellY + 0.5;
                viewAngle = facing * kPi / 2;
                Action next = pendingAction != Action::None ? pendingAction : heldAction();
                pendingAction = Action::None;
                startAction(next);
            }

            drawPseudo3D(renderer, *currentFloor, wallTexture, doorTexture, viewX, viewY, viewAngle);
            if (warpFlashStart && currentTime - warpFlashStart < kWarpFlashMs) {
                // ワープ直後: 白い閃光が引いていく
                double fade = 1.0 - (currentTime - warpFlashStart) / kWarpFlashMs;
                SDL_Rect view{0, 0, kViewWidth, kViewHeight};
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 235, 225, 255, static_cast<Uint8>(fade * fade * 255));
                SDL_RenderFillRect(renderer, &view);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            }
            // 看板は論理位置（入力時に確定済み）で判定し、移動・回転が半分進んだら出し始める。
            // 次の移動を始めると論理位置が変わるので、すぐ消える
            const std::string* sign = nullptr;
            if (!motion.active || motion.progress >= 0.5) {
                auto found = currentFloor->signs[facing].find(edgeKey(cellX, cellY));
                if (found != currentFloor->signs[facing].end()) sign = &found->second;
            }
            if (sign != shownSign) {
                shownSign = sign;
                signStart = currentTime;
            }
            if (sign) drawSign(renderer, font, textCache, *sign, (currentTime - signStart) / kSignFadeMs);
            drawPartyPanel(renderer, font, textCache, portraits, state.party);
            drawMessageWindow(renderer, font, textCache, portraits, database.characters, messages, currentTime);
            SDL_RenderPresent(renderer);
            drawMapPreview(previewRenderer, *currentFloor, map, state.doneEvents, viewX, viewY, viewAngle);
        }

        messages.release();
        textCache.clear();
        portraits.release();
        TTF_CloseFont(font);
        TTF_Quit();

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
