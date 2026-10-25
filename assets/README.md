# maze3d Assets

## Directory layout

- `textures/wall/` - wall textures
- `textures/door/` - door textures
- `textures/floor/` - floor textures
- `textures/stair/` - stair textures

## Naming

- Use lowercase snake_case.
- Keep the exported texture square.
- For pixel-art textures, use a texture size of 64x64 or 128x128.
- Use PNG for textures that may need transparency.

## Game texture names

The JSON `texture` field should use the file stem, for example:

```json
{ "axis": "h", "locate": [0, 0], "type": "wall", "length": 4, "texture": "stone_dark" }
```

The default wall texture is:

```text
assets/textures/wall/stone_dark.png
```

Generated images should include source, license, and generation conditions in `assets/LICENSES.md` when reused from an external service.
