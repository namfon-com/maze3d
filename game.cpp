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

struct Hit {
    bool hit = false;
    std::string type;
    bool horizontal = false;
    double distance = 0;
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

Hit castRay(const Floor& floor, double playerX, double playerY,
            double rayDirectionX, double rayDirectionY) {
    int mapX = static_cast<int>(std::floor(playerX));
    int mapY = static_cast<int>(std::floor(playerY));
    const double deltaX = rayDirectionX == 0 ? 1e30 : std::abs(1.0 / rayDirectionX);
    const double deltaY = rayDirectionY == 0 ? 1e30 : std::abs(1.0 / rayDirectionY);
    int stepX = rayDirectionX < 0 ? -1 : 1;
    int stepY = rayDirectionY < 0 ? -1 : 1;
    double sideX = rayDirectionX < 0
        ? (playerX - mapX) * deltaX
        : (mapX + 1.0 - playerX) * deltaX;
    double sideY = rayDirectionY < 0
        ? (playerY - mapY) * deltaY
        : (mapY + 1.0 - playerY) * deltaY;

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

        if (mapX < 0 || mapX >= floor.width || mapY < 0 || mapY >= floor.height) {
            return {true, "wall", verticalSide, std::max(rawDistance, 0.001), 0.0};
        }

        if (verticalSide) {
            int boundaryX = stepX > 0 ? mapX : mapX + 1;
            std::string edge = vEdge(floor, boundaryX, mapY);
            if (!edge.empty()) {
                double wallX = playerY + rawDistance * rayDirectionY;
                wallX -= std::floor(wallX);
                return {true, edge, false, std::max(rawDistance, 0.001), wallX};
            }
        } else {
            int boundaryY = stepY > 0 ? mapY : mapY + 1;
            std::string edge = hEdge(floor, mapX, boundaryY);
            if (!edge.empty()) {
                double wallX = playerX + rawDistance * rayDirectionX;
                wallX -= std::floor(wallX);
                return {true, edge, true, std::max(rawDistance, 0.001), wallX};
            }
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

void drawPseudo3D(SDL_Renderer* renderer, const Floor& floor, const TextureAsset& floorTexture,
                  const TextureAsset& wallTexture, const TextureAsset& doorTexture,
                  double playerX, double playerY, double directionX, double directionY) {
    const int width = 960;
    const int height = 600;
    SDL_SetRenderDrawColor(renderer, 20, 18, 16, 255);
    SDL_RenderClear(renderer);

    SDL_Rect ceiling{0, 0, width, height / 2};
    SDL_SetRenderDrawColor(renderer, 36, 34, 31, 255);
    SDL_RenderFillRect(renderer, &ceiling);

    if (floorTexture.texture) {
        SDL_Rect floorRect{0, height / 2, width, height / 2};
        SDL_RenderCopy(renderer, floorTexture.texture, nullptr, &floorRect);
    } else {
        SDL_Rect floorRect{0, height / 2, width, height / 2};
        SDL_SetRenderDrawColor(renderer, 12, 12, 12, 255);
        SDL_RenderFillRect(renderer, &floorRect);
    }

    const double planeX = -directionY * 0.66;
    const double planeY = directionX * 0.66;
    for (int column = 0; column < width; ++column) {
        double camera = 2.0 * column / width - 1.0;
        double rayX = directionX + planeX * camera;
        double rayY = directionY + planeY * camera;
        Hit hit = castRay(floor, playerX, playerY, rayX, rayY);
        if (!hit.hit || hit.distance <= 0.001) continue;
        double rayLength = std::sqrt(rayX * rayX + rayY * rayY);
        double perpendicularDistance = hit.distance;
        if (rayLength > 0.001) {
            perpendicularDistance *= std::max(0.0, (directionX * rayX + directionY * rayY) / rayLength);
        }
        int wallHeight = static_cast<int>(height / std::max(perpendicularDistance, 0.001) * 0.9);
        int top = std::max(0, height / 2 - wallHeight / 2);
        int bottom = std::min(height, height / 2 + wallHeight / 2);
        TextureAsset texture = hit.type == "door" ? doorTexture : wallTexture;
        SDL_Rect stripe{column, top, 1, bottom - top};
        if (texture.texture) {
            int sourceX = std::clamp(static_cast<int>(hit.wallX * texture.width), 0, texture.width - 1);
            SDL_Rect source{sourceX, 0, 1, texture.height};
            SDL_RenderCopy(renderer, texture.texture, &source, &stripe);
        } else {
            if (hit.type == "door") SDL_SetRenderDrawColor(renderer, 230, 190, 70, 255);
            else SDL_SetRenderDrawColor(renderer, 95, 91, 86, 255);
            SDL_RenderFillRect(renderer, &stripe);
        }
    }
}

void drawMapPreview(SDL_Renderer* renderer, const Floor& floor, const MapData& map,
                   double playerX, double playerY, double directionX, double directionY) {
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
    SDL_SetRenderDrawColor(renderer, 248, 246, 238, 255);
    SDL_RenderFillRect(renderer, &background);

    SDL_SetRenderDrawColor(renderer, 190, 188, 180, 255);
    for (int x = 0; x <= floor.width; ++x) {
        SDL_RenderDrawLine(renderer, originX + x * cellSize, originY,
                           originX + x * cellSize, originY + gridHeight);
    }
    for (int y = 0; y <= floor.height; ++y) {
        SDL_RenderDrawLine(renderer, originX, originY + y * cellSize,
                           originX + gridWidth, originY + y * cellSize);
    }

    for (const auto& edge : floor.hEdges) {
        int x = static_cast<int>(edge.first >> 32);
        int y = static_cast<int>(edge.first & 0xffffffffu);
        if (edge.second == "door") SDL_SetRenderDrawColor(renderer, 230, 190, 70, 255);
        else SDL_SetRenderDrawColor(renderer, 38, 38, 38, 255);
        SDL_RenderDrawLine(renderer, originX + x * cellSize, originY + y * cellSize,
                           originX + (x + 1) * cellSize, originY + y * cellSize);
    }
    for (const auto& edge : floor.vEdges) {
        int x = static_cast<int>(edge.first >> 32);
        int y = static_cast<int>(edge.first & 0xffffffffu);
        if (edge.second == "door") SDL_SetRenderDrawColor(renderer, 230, 190, 70, 255);
        else SDL_SetRenderDrawColor(renderer, 38, 38, 38, 255);
        SDL_RenderDrawLine(renderer, originX + x * cellSize, originY + y * cellSize,
                           originX + x * cellSize, originY + (y + 1) * cellSize);
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

    SDL_SetRenderDrawColor(renderer, 20, 65, 150, 255);
    int arrowX = static_cast<int>(playerCenterX + directionX * cellSize * 0.65);
    int arrowY = static_cast<int>(playerCenterY + directionY * cellSize * 0.65);
    for (int offset = -1; offset <= 1; ++offset) {
        SDL_RenderDrawLine(renderer, static_cast<int>(playerCenterX) - offset, static_cast<int>(playerCenterY) + offset,
                           arrowX - offset, arrowY + offset);
    }

    SDL_SetRenderDrawColor(renderer, 245, 245, 240, 255);

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

int main(int argc, char* argv[]) {
    fs::path exeDir = executableDirectory();
    try {
        MapData map = loadMap(findMapPath(argc, argv, exeDir));
        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
        if ((IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & (IMG_INIT_PNG | IMG_INIT_JPG)) == 0)
            throw std::runtime_error(IMG_GetError());

        SDL_Window* window = SDL_CreateWindow("maze3d - cell movement / 90 degree turn", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                               960, 600, SDL_WINDOW_SHOWN);
        if (!window) throw std::runtime_error(SDL_GetError());
        SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer) throw std::runtime_error(SDL_GetError());

        SDL_Window* previewWindow = SDL_CreateWindow("map preview - current position", SDL_WINDOWPOS_UNDEFINED,
                                                     SDL_WINDOWPOS_UNDEFINED, 420, 450, SDL_WINDOW_SHOWN);
        if (!previewWindow) throw std::runtime_error(SDL_GetError());
        SDL_Renderer* previewRenderer = SDL_CreateRenderer(previewWindow, -1, SDL_RENDERER_ACCELERATED);
        if (!previewRenderer) {
            SDL_DestroyWindow(previewWindow);
            throw std::runtime_error(SDL_GetError());
        }

        std::string currentFloorId = map.spawn.floorId;
        const Floor* currentFloor = findFloor(map, currentFloorId);
        if (!currentFloor && !map.floors.empty()) {
            currentFloor = &map.floors.front();
            currentFloorId = currentFloor->id;
        }
        if (!currentFloor) throw std::runtime_error("Map contains no valid floor");
        double playerX = map.spawn.x + 0.5;
        double playerY = map.spawn.y + 0.5;
        double directionX = -1.0;
        double directionY = 0.0;
        bool moving = false;
        double moveProgress = 0.0;
        double moveFromX = playerX;
        double moveFromY = playerY;
        double moveToX = playerX;
        double moveToY = playerY;

        bool turning = false;
        double turnProgress = 0.0;
        double turnFromX = directionX;
        double turnFromY = directionY;
        double turnToX = directionX;
        double turnToY = directionY;

        fs::path base = exeDir / "assets/textures";
        if (!fs::exists(base)) base = fs::path("assets/textures");
        fs::path floorImage = findAsset(base / "floor", map.spawn.floorId + ".png");
        fs::path wallImage = findAsset(base / "wall", "stone_dark.png");
        fs::path doorImage = findAsset(base / "door", "door.png");
        TextureAsset floorTexture = loadTexture(renderer, floorImage);
        TextureAsset wallTexture = loadTexture(renderer, wallImage);
        TextureAsset doorTexture = loadTexture(renderer, doorImage);

        bool running = true;
        Uint32 previousTime = SDL_GetTicks();
        while (running) {
            Uint32 currentTime = SDL_GetTicks();
            double frameTime = (currentTime - previousTime) / 1000.0;
            previousTime = currentTime;
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) running = false;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
                if (event.type == SDL_KEYDOWN) {
                    if (event.key.keysym.scancode == SDL_SCANCODE_Q) running = false;
                    if (!moving && !turning && !event.key.repeat) {
                        if (event.key.keysym.scancode == SDL_SCANCODE_LEFT || event.key.keysym.scancode == SDL_SCANCODE_A) {
                            turnFromX = directionX;
                            turnFromY = directionY;
                            turnToX = directionY;
                            turnToY = -directionX;
                            double length = std::sqrt(turnToX * turnToX + turnToY * turnToY);
                            if (length > 0.001) {
                                turnToX /= length;
                                turnToY /= length;
                            }
                            turnProgress = 0.0;
                            turning = true;
                        }
                        if (event.key.keysym.scancode == SDL_SCANCODE_RIGHT || event.key.keysym.scancode == SDL_SCANCODE_D) {
                            turnFromX = directionX;
                            turnFromY = directionY;
                            turnToX = -directionY;
                            turnToY = directionX;
                            double length = std::sqrt(turnToX * turnToX + turnToY * turnToY);
                            if (length > 0.001) {
                                turnToX /= length;
                                turnToY /= length;
                            }
                            turnProgress = 0.0;
                            turning = true;
                        }
                        if (event.key.keysym.scancode == SDL_SCANCODE_DOWN || event.key.keysym.scancode == SDL_SCANCODE_S) {
                            turnFromX = directionX;
                            turnFromY = directionY;
                            turnToX = -directionX;
                            turnToY = -directionY;
                            turnProgress = 0.0;
                            turning = true;
                        }
                        int forwardSign = 0;
                        if (event.key.keysym.scancode == SDL_SCANCODE_UP || event.key.keysym.scancode == SDL_SCANCODE_W) forwardSign = 1;
                        if (forwardSign != 0) {
                            int baseX = static_cast<int>(std::floor(playerX));
                            int baseY = static_cast<int>(std::floor(playerY));
                            int directionStepX = std::abs(directionX) > 0.5 ? (directionX > 0 ? 1 : -1) : 0;
                            int directionStepY = std::abs(directionY) > 0.5 ? (directionY > 0 ? 1 : -1) : 0;
                            int stepX = directionStepX * forwardSign;
                            int stepY = directionStepY * forwardSign;
                            if (!moveBlocked(*currentFloor, baseX, baseY, stepX, stepY)) {
                                double targetX = baseX + 0.5 + stepX;
                                double targetY = baseY + 0.5 + stepY;
                                moveFromX = playerX;
                                moveFromY = playerY;
                                moveToX = targetX;
                                moveToY = targetY;
                                moveProgress = 0.0;
                                moving = true;
                            }
                        }
                    }
                    if (event.key.keysym.scancode == SDL_SCANCODE_E) {
                        int cellX = static_cast<int>(std::floor(playerX));
                        int cellY = static_cast<int>(std::floor(playerY));
                        for (const Stair& stair : map.stairs) {
                            for (const StairEnd& end : stair.ends) {
                                if (end.floorId == currentFloorId && end.x == cellX && end.y == cellY) {
                                    for (const StairEnd& destination : stair.ends) {
                                        if (destination.floorId == currentFloorId && destination.x == cellX && destination.y == cellY) continue;
                                        currentFloorId = destination.floorId;
                                        currentFloor = findFloor(map, currentFloorId);
                                        playerX = destination.x + 0.5;
                                        playerY = destination.y + 0.5;
                                        break;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            if (moving) {
                moveProgress = std::min(1.0, moveProgress + std::min(frameTime, 0.05) / 0.18);
                double eased = moveProgress * moveProgress * (3.0 - 2.0 * moveProgress);
                playerX = moveFromX + (moveToX - moveFromX) * eased;
                playerY = moveFromY + (moveToY - moveFromY) * eased;
                if (moveProgress >= 1.0) {
                    playerX = moveToX;
                    playerY = moveToY;
                    moving = false;
                }
            }

            if (turning) {
                turnProgress = std::min(1.0, turnProgress + std::min(frameTime, 0.05) / 0.16);
                double eased = turnProgress * turnProgress * (3.0 - 2.0 * turnProgress);
                double currentX = turnFromX + (turnToX - turnFromX) * eased;
                double currentY = turnFromY + (turnToY - turnFromY) * eased;
                double length = std::sqrt(currentX * currentX + currentY * currentY);
                if (length > 0.001) {
                    directionX = currentX / length;
                    directionY = currentY / length;
                }
                if (turnProgress >= 1.0) {
                    directionX = turnToX;
                    directionY = turnToY;
                    double cardinalLength = std::sqrt(directionX * directionX + directionY * directionY);
                    if (cardinalLength > 0.001) {
                        directionX /= cardinalLength;
                        directionY /= cardinalLength;
                    }
                    turning = false;
                }
            }

            drawPseudo3D(renderer, *currentFloor, floorTexture, wallTexture, doorTexture,
                         playerX, playerY, directionX, directionY);
            SDL_RenderPresent(renderer);
            drawMapPreview(previewRenderer, *currentFloor, map, playerX, playerY, directionX, directionY);
        }

        if (floorTexture.texture) SDL_DestroyTexture(floorTexture.texture);
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
