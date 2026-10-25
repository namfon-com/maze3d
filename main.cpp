#include <SDL2/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

struct StairEnd
{
    std::string floorId;
    int x = 0;
    int y = 0;
};

struct Stair
{
    std::string id;
    StairEnd ends[2];
};

struct Edge
{
    bool horizontal = false;
    int x = 0;
    int y = 0;
    std::string type;
};

struct Floor
{
    std::string id;
    int width = 0;
    int height = 0;
    std::unordered_map<std::uint64_t, std::string> hEdges;
    std::unordered_map<std::uint64_t, std::string> vEdges;
};

struct MapData
{
    int version = 0;
    std::vector<Floor> floors;
    std::vector<Stair> stairs;
    StairEnd spawn;
};

std::uint64_t edgeKey(int x, int y)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32)
        | static_cast<std::uint32_t>(y);
}

int getVersion(const json& document)
{
    if (!document.is_object() || !document.contains("version") || !document["version"].is_number_integer())
    {
        throw std::runtime_error("Invalid or missing map version.");
    }
    return document["version"].get<int>();
}

MapData loadMap(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input)
    {
        throw std::runtime_error("Cannot open map: " + path.string());
    }

    json document;
    input >> document;
    if (getVersion(document) != 1)
    {
        throw std::runtime_error("Unsupported map version: " + std::to_string(getVersion(document)));
    }
    if (!document.contains("floors") || !document["floors"].is_array() || document["floors"].empty())
    {
        throw std::runtime_error("Map has no floors.");
    }

    MapData data;
    data.version = 1;

    for (const auto& floorJson : document["floors"])
    {
        Floor floor;
        floor.id = floorJson.at("id").get<std::string>();
        floor.width = floorJson.at("width").get<int>();
        floor.height = floorJson.at("height").get<int>();

        if (floor.width < 1 || floor.height < 1)
        {
            throw std::runtime_error("Invalid floor size: " + floor.id);
        }

        for (const auto& edgeJson : floorJson.value("edges", json::array()))
        {
            const std::string axis = edgeJson.at("axis").get<std::string>();
            const std::string type = edgeJson.at("type").get<std::string>();
            const auto locate = edgeJson.at("locate");
            const int x = locate.at(0).get<int>();
            const int y = locate.at(1).get<int>();
            const int length = edgeJson.value("length", 1);

            for (int offset = 0; offset < length; ++offset)
            {
                if (axis == "h")
                {
                    const int edgeX = x + offset;
                    if (edgeX >= 0 && edgeX < floor.width && y >= 0 && y <= floor.height)
                    {
                        floor.hEdges[edgeKey(edgeX, y)] = type;
                    }
                }
                else if (axis == "v")
                {
                    const int edgeY = y + offset;
                    if (x >= 0 && x <= floor.width && edgeY >= 0 && edgeY < floor.height)
                    {
                        floor.vEdges[edgeKey(x, edgeY)] = type;
                    }
                }
            }
        }
        data.floors.push_back(std::move(floor));
    }

    if (document.contains("stairs") && document["stairs"].is_array())
    {
        for (const auto& stairJson : document["stairs"])
        {
            Stair stair;
            stair.id = stairJson.at("id").get<std::string>();
            for (int i = 0; i < 2; ++i)
            {
                const auto& end = stairJson.at("ends").at(i);
                stair.ends[i].floorId = end.at("floorId").get<std::string>();
                stair.ends[i].x = end.at("locate").at(0).get<int>();
                stair.ends[i].y = end.at("locate").at(1).get<int>();
            }
            data.stairs.push_back(stair);
        }
    }

    if (document.contains("spawn"))
    {
        const auto& spawn = document["spawn"];
        data.spawn.floorId = spawn.at("floorId").get<std::string>();
        data.spawn.x = spawn.at("locate").at(0).get<int>();
        data.spawn.y = spawn.at("locate").at(1).get<int>();
    }

    return data;
}

const Floor* findFloor(const MapData& data, const std::string& floorId)
{
    const auto found = std::find_if(data.floors.begin(), data.floors.end(),
        [&floorId](const Floor& floor) { return floor.id == floorId; });
    return found == data.floors.end() ? nullptr : &*found;
}

void drawMap(SDL_Renderer* renderer, const Floor& floor, const MapData& data)
{
    constexpr int originX = 40;
    constexpr int originY = 40;
    constexpr int cell = 32;

    SDL_SetRenderDrawColor(renderer, 235, 232, 224, 255);
    SDL_RenderClear(renderer);

    SDL_Rect rectangle { originX, originY, floor.width * cell, floor.height * cell };
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderFillRect(renderer, &rectangle);

    SDL_SetRenderDrawColor(renderer, 190, 190, 190, 255);
    for (int x = 0; x <= floor.width; ++x)
    {
        SDL_RenderDrawLine(renderer, originX + x * cell, originY,
            originX + x * cell, originY + floor.height * cell);
    }
    for (int y = 0; y <= floor.height; ++y)
    {
        SDL_RenderDrawLine(renderer, originX, originY + y * cell,
            originX + floor.width * cell, originY + y * cell);
    }

    for (const auto& edge : floor.hEdges)
    {
        const int x = static_cast<int>(edge.first >> 32);
        const int y = static_cast<int>(edge.first & 0xffffffffu);
        SDL_SetRenderDrawColor(renderer, edge.second == "door" ? 192 : 38,
            edge.second == "door" ? 90 : 38,
            edge.second == "door" ? 56 : 38, 255);
        SDL_RenderDrawLine(renderer, originX + x * cell, originY + y * cell,
            originX + (x + 1) * cell, originY + y * cell);
    }
    for (const auto& edge : floor.vEdges)
    {
        const int x = static_cast<int>(edge.first >> 32);
        const int y = static_cast<int>(edge.first & 0xffffffffu);
        SDL_SetRenderDrawColor(renderer, edge.second == "door" ? 192 : 38,
            edge.second == "door" ? 90 : 38,
            edge.second == "door" ? 56 : 38, 255);
        SDL_RenderDrawLine(renderer, originX + x * cell, originY + y * cell,
            originX + x * cell, originY + (y + 1) * cell);
    }

    for (const Stair& stair : data.stairs)
    {
        for (const StairEnd& end : stair.ends)
        {
            if (end.floorId != floor.id)
            {
                continue;
            }
            SDL_Rect marker {
                originX + end.x * cell + cell / 2 - 8,
                originY + end.y * cell + cell / 2 - 8,
                16, 16
            };
            SDL_SetRenderDrawColor(renderer, 231, 168, 62, 255);
            SDL_RenderFillRect(renderer, &marker);
        }
    }

    if (data.spawn.floorId == floor.id)
    {
        SDL_Rect spawn {
            originX + data.spawn.x * cell + cell / 2 - 10,
            originY + data.spawn.y * cell + cell / 2 - 10,
            20, 20
        };
        SDL_SetRenderDrawColor(renderer, 91, 141, 239, 255);
        SDL_RenderFillRect(renderer, &spawn);
    }
}

std::filesystem::path findMapPath(int argc, char* argv[])
{
    if (argc >= 2)
    {
        return argv[1];
    }

    const std::vector<std::filesystem::path> candidates {
        "map_editor/mymap00.json",
        "../map_editor/mymap00.json",
        "../../map_editor/mymap00.json",
        "map_editor/mymap00.json"
    };
    for (const std::filesystem::path& candidate : candidates)
    {
        if (std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }
    return candidates.front();
}

int main(int argc, char* argv[])
{
    try
    {
        const MapData data = loadMap(findMapPath(argc, argv));
        if (SDL_Init(SDL_INIT_VIDEO) != 0)
        {
            throw std::runtime_error(SDL_GetError());
        }

        SDL_Window* window = SDL_CreateWindow(
            "maze3d - map preview",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            800,
            600,
            SDL_WINDOW_SHOWN
        );
        if (!window)
        {
            throw std::runtime_error(SDL_GetError());
        }

        SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer)
        {
            SDL_DestroyWindow(window);
            throw std::runtime_error(SDL_GetError());
        }

        const Floor* currentFloor = findFloor(data, data.spawn.floorId);
        if (!currentFloor && !data.floors.empty())
        {
            currentFloor = &data.floors.front();
        }

        std::cout << "Loaded map version " << data.version << ": " << data.floors.size()
                  << " floors, " << data.stairs.size() << " stairs" << std::endl;
        if (currentFloor)
        {
            std::cout << "Preview floor: " << currentFloor->id << std::endl;
        }

        bool running = true;
        while (running && currentFloor)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_QUIT)
                {
                    running = false;
                }
            }

            drawMap(renderer, *currentFloor, data);
            SDL_RenderPresent(renderer);
        }

        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "maze3d: " << error.what() << std::endl;
        return 1;
    }
}
