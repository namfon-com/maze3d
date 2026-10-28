from __future__ import annotations

import json
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, simpledialog, ttk
from typing import Any


SCHEMA_VERSION = 1
CELL_SIZE = 48
GRID_ORIGIN = 40
MIN_MAP_SIZE = 1
MAX_MAP_SIZE = 200
EDGE_TYPES = {"wall", "door", "oneway"}
# 一方通行の壁はエディタ内部で "oneway:E" のように通れる向き付きで持つ（JSON では type + pass）
ONEWAY_PREFIX = "oneway:"
ONEWAY_COLOR = "#c0392b"
OPPOSITE_FACING = {"N": "S", "S": "N", "E": "W", "W": "E"}
WARP_COLOR = "#6a3fb5"
DOOR_COLOR = "#f2c230"
DOOR_OUTLINE = "#8a6400"
SIGN_COLOR = "#fff1c8"
SIGN_TEXT = "#6b4a00"
# 開始時の向き（時計回り順）。y はマップ下方向が正
FACINGS = ["N", "E", "S", "W"]
FACING_VECTORS = {"N": (0, -1), "E": (1, 0), "S": (0, 1), "W": (-1, 0)}
FACING_LABELS = {"N": "北", "E": "東", "S": "南", "W": "西"}
# イベントマスの種類
EVENT_TYPES = {"message": "メッセージ", "treasure": "宝箱", "recruit": "人間（勧誘）", "warp": "ワープ"}
KEEP_FACING_LABEL = "そのまま"
# ゲーム本体と共有するキャラクター定義（勧誘イベントの人物リストに使う）
CHARACTERS_PATH = Path(__file__).resolve().parent.parent / "assets" / "data" / "characters.json"


def load_character_choices() -> list[tuple[str, str]]:
    """characters.json から (id, 名前) の一覧を返す。主人公は除く。読めなければ空。"""
    try:
        document = json.loads(CHARACTERS_PATH.read_text(encoding="utf-8"))
        hero = document.get("hero")
        return [(c["id"], c.get("name", c["id"])) for c in document.get("characters", [])
                if isinstance(c, dict) and "id" in c and c["id"] != hero]
    except (OSError, ValueError, TypeError, KeyError):
        return []


def validate_size(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{label} は整数で指定してください。")
    if not MIN_MAP_SIZE <= value <= MAX_MAP_SIZE:
        raise ValueError(f"{label} は {MIN_MAP_SIZE}〜{MAX_MAP_SIZE} の範囲で指定してください。")
    return value


def parse_coordinate(value: Any, label: str) -> tuple[int, int]:
    if (not isinstance(value, list) or len(value) != 2
            or any(isinstance(part, bool) or not isinstance(part, int) for part in value)):
        raise ValueError(f"{label}.locate は整数2個の配列で指定してください。")
    return value[0], value[1]


def parse_event(raw: Any, label: str) -> dict[str, Any]:
    """JSONのイベント定義を検証し、locate を除いた正規化済みの辞書を返す。"""
    if not isinstance(raw, dict):
        raise ValueError(f"{label} はオブジェクトではありません。")
    kind = raw.get("type")
    if kind not in EVENT_TYPES:
        raise ValueError(f"{label}.type は {' / '.join(EVENT_TYPES)} のいずれかです。")
    text = raw.get("text", "")
    if not isinstance(text, str):
        raise ValueError(f"{label}.text は文字列で指定してください。")
    event: dict[str, Any] = {"type": kind, "text": text}
    if kind == "message":
        once = raw.get("once", False)
        if not isinstance(once, bool):
            raise ValueError(f"{label}.once は true / false で指定してください。")
        event["once"] = once
    elif kind == "recruit":
        character = raw.get("character")
        if not isinstance(character, str) or not character:
            raise ValueError(f"{label}.character にキャラクターIDを指定してください。")
        event["character"] = character
    elif kind == "warp":
        to = raw.get("to")
        if not isinstance(to, dict) or not isinstance(to.get("floorId"), str):
            raise ValueError(f"{label}.to に行き先（floorId, locate）を指定してください。")
        tx, ty = parse_coordinate(to.get("locate"), f"{label}.to")
        event["to"] = {"floorId": to["floorId"], "locate": [tx, ty]}
        if "facing" in to:
            if to["facing"] not in FACINGS:
                raise ValueError(f"{label}.to.facing は N / E / S / W のいずれかで指定してください。")
            event["to"]["facing"] = to["facing"]
    else:
        gold = raw.get("gold", 0)
        item = raw.get("item", "")
        if isinstance(gold, bool) or not isinstance(gold, int) or gold < 0:
            raise ValueError(f"{label}.gold は0以上の整数で指定してください。")
        if not isinstance(item, str):
            raise ValueError(f"{label}.item は文字列で指定してください。")
        event["gold"] = gold
        event["item"] = item
    return event


class MapCanvas(tk.Canvas):
    def __init__(self, master: tk.Misc, editor: "MapEditor", **kwargs: Any) -> None:
        super().__init__(master, background="#fbfaf7", highlightthickness=0, **kwargs)
        self.editor = editor
        self.press_point: tuple[float, float] | None = None
        self.last_grid_vertex: tuple[int, int] | None = None
        self.dragged = False
        self.bind("<Button-1>", self._press)
        self.bind("<B1-Motion>", self._drag)
        self.bind("<ButtonRelease-1>", self._release)
        self.bind("<MouseWheel>", self._wheel_floor)
        self.bind("<Motion>", self._hover)
        self.bind("<Leave>", lambda _event: self.editor.cursor_cell.set(""))
        self.bind("<Configure>", lambda _event: self.redraw())

    def _press(self, event: tk.Event) -> None:
        self.press_point = (self.canvasx(event.x), self.canvasy(event.y))
        self.last_grid_vertex = None
        self.dragged = False

    def _drag(self, event: tk.Event) -> None:
        if self.press_point is None:
            return
        x, y = self.canvasx(event.x), self.canvasy(event.y)
        dx, dy = x - self.press_point[0], y - self.press_point[1]
        if self.editor.tool.get() in ("wall", "door"):
            if not self.dragged and max(abs(dx), abs(dy)) >= 7:
                self.dragged = True
                start_gx, start_gy = self._grid_position(*self.press_point)
                self.last_grid_vertex = (round(start_gx), round(start_gy))
            if self.dragged:
                self._continue_edge_drag(x, y)
        elif self.editor.tool.get() == "erase":
            self.dragged = True
            self._apply_erase(x, y)

    def _hover(self, event: tk.Event) -> None:
        gx, gy = self._grid_position(self.canvasx(event.x), self.canvasy(event.y))
        if gx >= 0 and gy >= 0 and self._in_work_area(gx, gy):
            self.editor.cursor_cell.set(f"{self.editor.current_floor['id']}  x {int(gx)}, y {int(gy)}")
        else:
            self.editor.cursor_cell.set("")

    def _wheel_floor(self, event: tk.Event) -> str:
        self.editor.switch_floor_by_wheel(event.delta)
        return "break"

    def _release(self, event: tk.Event) -> None:
        if self.press_point is None:
            return
        x, y = self.canvasx(event.x), self.canvasy(event.y)
        tool = self.editor.tool.get()
        if tool == "wall" or tool == "door":
            if self.dragged:
                self._continue_edge_drag(x, y)
            else:
                self._place_nearest_edge(x, y, "wall" if tool == "wall" else "door")
        elif tool == "oneway":
            self._place_oneway(x, y)
        elif tool == "stair":
            self._place_cell_tool(x, y, self.editor.place_stair)
        elif tool == "warp":
            self._place_cell_tool(x, y, self.editor.place_warp)
        elif tool == "spawn":
            self._place_cell_tool(x, y, self.editor.place_spawn)
        elif tool == "event":
            self._place_cell_tool(x, y, self.editor.edit_event)
        elif tool == "sign":
            gx, gy = self._grid_position(x, y)
            if self._in_work_area(gx, gy) and gx >= 0 and gy >= 0:
                self.editor.edit_sign((int(gx), int(gy)), self._nearest_side(gx, gy))
        elif tool == "erase" and not self.dragged:
            self._apply_erase(x, y)
        self.press_point = None
        self.last_grid_vertex = None
        self.dragged = False

    def _grid_position(self, x: float, y: float) -> tuple[float, float]:
        return (x - GRID_ORIGIN) / CELL_SIZE, (y - GRID_ORIGIN) / CELL_SIZE

    def _in_work_area(self, gx: float, gy: float) -> bool:
        floor = self.editor.current_floor
        work_width = min(MAX_MAP_SIZE, floor["width"] + 4)
        work_height = min(MAX_MAP_SIZE, floor["height"] + 4)
        return -0.5 <= gx <= work_width + 0.5 and -0.5 <= gy <= work_height + 0.5

    def _continue_edge_drag(self, x: float, y: float) -> None:
        gx, gy = self._grid_position(x, y)
        if not self._in_work_area(gx, gy):
            return
        target = (round(gx), round(gy))
        if self.last_grid_vertex is None:
            self.last_grid_vertex = target
            return
        for start, end in self._grid_path(self.last_grid_vertex, target):
            if start[1] == end[1]:
                edge = ("h", min(start[0], end[0]), start[1])
            else:
                edge = ("v", start[0], min(start[1], end[1]))
            self._draw_edge(edge, "wall" if self.editor.tool.get() == "wall" else "door")
        self.last_grid_vertex = target

    @staticmethod
    def _grid_path(start: tuple[int, int], end: tuple[int, int]) -> list[
            tuple[tuple[int, int], tuple[int, int]]]:
        """Connect grid vertices with an axis-aligned path that follows the pointer."""
        x, y = start
        dx, dy = abs(end[0] - x), abs(end[1] - y)
        step_x = 1 if end[0] >= x else -1
        step_y = 1 if end[1] >= y else -1
        error = dx - dy
        path = []
        while (x, y) != end:
            doubled_error = 2 * error
            if doubled_error > -dy:
                error -= dy
                previous = (x, y)
                x += step_x
                path.append((previous, (x, y)))
            if doubled_error < dx:
                error += dx
                previous = (x, y)
                y += step_y
                path.append((previous, (x, y)))
        return path

    def _place_nearest_edge(self, x: float, y: float, kind: str) -> None:
        gx, gy = self._grid_position(x, y)
        if not self._in_work_area(gx, gy):
            return
        candidates = [
            (abs(gy - round(gy)) * CELL_SIZE,
             ("h", int(gx), round(gy))),
            (abs(gx - round(gx)) * CELL_SIZE,
             ("v", round(gx), int(gy))),
        ]
        distance, edge = min(candidates, key=lambda candidate: candidate[0])
        if distance <= CELL_SIZE * 0.38:
            self._draw_edge(edge, kind)

    def _label(self, x: float, y: float, text: str, color: str, background: str) -> None:
        """背景付きの小さな文字ラベル（線や記号と重なっても読めるように）。"""
        item = self.create_text(x, y, text=text, fill=color, font=("Segoe UI", 8, "bold"))
        x1, y1, x2, y2 = self.bbox(item)
        self.create_rectangle(x1 - 2, y1, x2 + 2, y2, fill=background, outline=color)
        self.tag_raise(item)

    def _place_oneway(self, x: float, y: float) -> None:
        """一方通行の壁を置く。クリックした側から反対側へ通れる。既存の一方通行壁をクリックすると向きを反転。"""
        gx, gy = self._grid_position(x, y)
        if not self._in_work_area(gx, gy):
            return
        candidates = [
            (abs(gy - round(gy)) * CELL_SIZE, ("h", int(gx), round(gy))),
            (abs(gx - round(gx)) * CELL_SIZE, ("v", round(gx), int(gy))),
        ]
        distance, edge = min(candidates, key=lambda candidate: candidate[0])
        if distance > CELL_SIZE * 0.38 or not MapEditor._edge_in_bounds(edge, MAX_MAP_SIZE, MAX_MAP_SIZE):
            return
        floor = self.editor.current_floor
        current = floor["edges"].get(edge, "")
        if current.startswith(ONEWAY_PREFIX):
            passing = OPPOSITE_FACING[current[len(ONEWAY_PREFIX):]]
        elif edge[0] == "h":
            passing = "S" if gy < edge[2] else "N"
        else:
            passing = "E" if gx < edge[1] else "W"
        floor["edges"][edge] = ONEWAY_PREFIX + passing
        self.editor.expand_for_edge(edge)
        self.editor.mark_changed()
        self.editor.status.set(f"一方通行の壁: {FACING_LABELS[passing]}向きにだけ通れます。再クリックで反転。")
        self.redraw()

    def _draw_edge(self, edge: tuple[str, int, int], kind: str) -> None:
        floor = self.editor.current_floor
        if not MapEditor._edge_in_bounds(edge, MAX_MAP_SIZE, MAX_MAP_SIZE):
            return
        kind = "wall" if self.editor.tool.get() == "wall" else "door"
        if floor["edges"].get(edge) != kind:
            floor["edges"][edge] = kind
            self.editor.expand_for_edge(edge)
            self.editor.mark_changed()
            self.redraw()

    def _place_cell_tool(self, x: float, y: float, callback: Any) -> None:
        gx, gy = self._grid_position(x, y)
        if not self._in_work_area(gx, gy):
            return
        cell = (int(gx), int(gy))
        floor = self.editor.current_floor
        if 0 <= cell[0] < MAX_MAP_SIZE and 0 <= cell[1] < MAX_MAP_SIZE:
            callback(cell)

    @staticmethod
    def _nearest_side(gx: float, gy: float) -> str:
        """セル内のクリック位置から一番近い辺の向き（その辺を向いて立つ）。"""
        fx, fy = gx - int(gx), gy - int(gy)
        distances = {"N": fy, "S": 1 - fy, "W": fx, "E": 1 - fx}
        return min(distances, key=distances.get)

    def _apply_erase(self, x: float, y: float) -> None:
        gx, gy = self._grid_position(x, y)
        self.editor.erase_near(x, y, gx, gy)
        self.redraw()

    def redraw(self) -> None:
        if not self.winfo_exists():
            return
        self.delete("all")
        floor = self.editor.current_floor
        width = floor["width"]
        height = floor["height"]
        work_width = min(MAX_MAP_SIZE, width + 4)
        work_height = min(MAX_MAP_SIZE, height + 4)
        left = GRID_ORIGIN
        top = GRID_ORIGIN
        right = left + work_width * CELL_SIZE
        bottom = top + work_height * CELL_SIZE
        self.configure(scrollregion=(0, 0, right + GRID_ORIGIN, bottom + GRID_ORIGIN))

        for x in range(work_width + 1):
            px = left + x * CELL_SIZE
            color = "#d9d5cc" if x <= width else "#eeeae2"
            self.create_line(px, top, px, bottom, fill=color, width=1)
        for y in range(work_height + 1):
            py = top + y * CELL_SIZE
            color = "#d9d5cc" if y <= height else "#eeeae2"
            self.create_line(left, py, right, py, fill=color, width=1)

        current_right = left + width * CELL_SIZE
        current_bottom = top + height * CELL_SIZE
        self.create_rectangle(left, top, current_right, current_bottom,
                              outline="#91a2aa", width=2, dash=(4, 3))

        for (axis, x, y), kind in floor["edges"].items():
            if axis == "h":
                x1, y1 = left + x * CELL_SIZE, top + y * CELL_SIZE
                x2, y2 = x1 + CELL_SIZE, y1
            else:
                x1, y1 = left + x * CELL_SIZE, top + y * CELL_SIZE
                x2, y2 = x1, y1 + CELL_SIZE
            if kind == "door":
                # ドア: 細い線 + 中央の太いバー（扉板）
                self.create_line(x1, y1, x2, y2, fill=DOOR_OUTLINE, width=3)
                mx, my = (x1 + x2) / 2, (y1 + y2) / 2
                half_length, half_thickness = CELL_SIZE * 0.28, 5
                if axis == "h":
                    bar = (mx - half_length, my - half_thickness, mx + half_length, my + half_thickness)
                else:
                    bar = (mx - half_thickness, my - half_length, mx + half_thickness, my + half_length)
                self.create_rectangle(*bar, fill=DOOR_COLOR, outline=DOOR_OUTLINE, width=2)
            elif kind.startswith(ONEWAY_PREFIX):
                # 一方通行: 赤い破線の壁 + 通れる向きの矢印
                self.create_line(x1, y1, x2, y2, fill=ONEWAY_COLOR, width=5, dash=(8, 4))
                dx, dy = FACING_VECTORS[kind[len(ONEWAY_PREFIX):]]
                mx, my = (x1 + x2) / 2, (y1 + y2) / 2
                self.create_line(mx - dx * 9, my - dy * 9, mx + dx * 13, my + dy * 13,
                                 fill=ONEWAY_COLOR, width=3, arrow=tk.LAST, arrowshape=(9, 11, 4))
            else:
                self.create_line(x1, y1, x2, y2, fill="#263238", width=5, capstyle=tk.ROUND)

        for (event_x, event_y), event in floor["events"].items():
            x1 = left + event_x * CELL_SIZE + 6
            y1 = top + event_y * CELL_SIZE + 6
            x2, y2 = x1 + CELL_SIZE - 12, y1 + CELL_SIZE - 12
            if event["type"] == "warp":
                # ワープ床: 紫の渦（同心円）と行き先
                cx, cy = (x1 + x2) / 2, (y1 + y2) / 2 - 5
                self.create_oval(cx - 14, cy - 14, cx + 14, cy + 14, fill="#e3d4f7", outline=WARP_COLOR, width=2)
                self.create_oval(cx - 7, cy - 7, cx + 7, cy + 7, outline=WARP_COLOR, width=2)
                self.create_oval(cx - 2, cy - 2, cx + 2, cy + 2, fill=WARP_COLOR, outline="")
                to = event["to"]
                destination = f"→{to['floorId']} {to['locate'][0]},{to['locate'][1]}"
                self._label(cx, y2 + 1, destination, WARP_COLOR, "#f4eefc")
            elif event["type"] == "recruit":
                self.create_rectangle(x1, y1, x2, y2, fill="#c9f0cc", outline="#2e7d32", width=2)
                self.create_text((x1 + x2) / 2, (y1 + y2) / 2, text="人", fill="#1b5e20",
                                 font=("Segoe UI", 12, "bold"))
            elif event["type"] == "treasure":
                self.create_rectangle(x1, y1 + 6, x2, y2, fill="#c98b3c", outline="#5c3a12", width=2)
                self.create_line(x1, y1 + 16, x2, y1 + 16, fill="#5c3a12", width=2)
                self.create_text((x1 + x2) / 2, y1 + 24, text="宝", fill="#2b1a06", font=("Segoe UI", 9, "bold"))
            else:
                self.create_rectangle(x1, y1, x2, y2, fill="#cfe8ff", outline="#2f6db5", width=2)
                self.create_text((x1 + x2) / 2, (y1 + y2) / 2, text="!", fill="#1d4f8f",
                                 font=("Segoe UI", 14, "bold"))

        for (sign_x, sign_y, facing), text in floor["signs"].items():
            # 看板: 見る向きの辺の内側に札（先頭の数文字を表示）
            dx, dy = FACING_VECTORS[facing]
            cx = left + (sign_x + 0.5 + dx * 0.3) * CELL_SIZE
            cy = top + (sign_y + 0.5 + dy * 0.3) * CELL_SIZE
            label = text.splitlines()[0] if text else ""
            label = label[:4] + ("…" if len(label) > 4 else "")
            item = self.create_text(cx, cy, text=label, fill=SIGN_TEXT, font=("Segoe UI", 7, "bold"))
            x1, y1, x2, y2 = self.bbox(item)
            self.create_rectangle(x1 - 2, y1, x2 + 2, y2, fill=SIGN_COLOR, outline=SIGN_TEXT)
            self.tag_raise(item)

        # ワープの着地点: 紫の破線の輪 + 「着」
        arrivals: dict[tuple[int, int], int] = {}
        for source in self.editor.floors:
            for event in source["events"].values():
                if event["type"] == "warp" and event["to"]["floorId"] == floor["id"]:
                    cell = tuple(event["to"]["locate"])
                    arrivals[cell] = arrivals.get(cell, 0) + 1
        for (arrive_x, arrive_y), count in arrivals.items():
            x1 = left + arrive_x * CELL_SIZE + 4
            y1 = top + arrive_y * CELL_SIZE + 4
            x2, y2 = x1 + CELL_SIZE - 8, y1 + CELL_SIZE - 8
            self.create_oval(x1, y1, x2, y2, outline=WARP_COLOR, width=2, dash=(4, 3))
            self._label(x2 - 4, y1 + 5, "着" if count == 1 else f"着×{count}", "white", WARP_COLOR)

        pending_warp = self.editor.pending_warp
        if pending_warp and pending_warp["floorId"] == floor["id"]:
            cx = left + (pending_warp["locate"][0] + 0.5) * CELL_SIZE
            cy = top + (pending_warp["locate"][1] + 0.5) * CELL_SIZE
            self.create_oval(cx - 15, cy - 15, cx + 15, cy + 15, fill="#c7a8f0", outline=WARP_COLOR, width=2)
            self.create_text(cx, cy, text="元", fill="#2c1557", font=("Segoe UI", 10, "bold"))

        for stair in self.editor.stairs:
            for end in stair["ends"]:
                if end["floorId"] != floor["id"]:
                    continue
                cx = left + (end["locate"][0] + 0.5) * CELL_SIZE
                cy = top + (end["locate"][1] + 0.5) * CELL_SIZE
                radius = 15
                self.create_oval(cx - radius, cy - radius, cx + radius, cy + radius,
                                 fill="#e7a83e", outline="#8a5a13", width=2)
                stair_number = stair["id"]
                if stair_number.startswith("stair_"):
                    stair_number = stair_number.removeprefix("stair_")
                icon = self.editor.stair_icon(stair, floor["id"])
                self.create_text(cx, cy - 5, text=icon,
                                 fill="#30230e", font=("Segoe UI", 8, "bold"))
                self.create_text(cx, cy + 6, text=f"St:{stair_number}",
                                 fill="#30230e", font=("Segoe UI", 7, "bold"))

        pending = self.editor.pending_stair
        if pending and pending["floorId"] == floor["id"]:
            cx = left + (pending["locate"][0] + 0.5) * CELL_SIZE
            cy = top + (pending["locate"][1] + 0.5) * CELL_SIZE
            self.create_oval(cx - 14, cy - 14, cx + 14, cy + 14,
                             fill="#76c7c4", outline="#176b69", width=2)
            self.create_text(cx, cy, text="1", fill="#103e3d", font=("Segoe UI", 11, "bold"))

        spawn = self.editor.spawn
        if spawn and spawn["floorId"] == floor["id"]:
            cx = left + (spawn["locate"][0] + 0.5) * CELL_SIZE
            cy = top + (spawn["locate"][1] + 0.5) * CELL_SIZE
            dx, dy = FACING_VECTORS[spawn.get("facing", "N")]
            # 向きを示す三角形（円の外側に突き出す）
            px, py = -dy, dx
            self.create_polygon(cx + dx * 22, cy + dy * 22,
                                cx + dx * 10 + px * 9, cy + dy * 10 + py * 9,
                                cx + dx * 10 - px * 9, cy + dy * 10 - py * 9,
                                fill="#244b91", outline="#244b91")
            self.create_oval(cx - 15, cy - 15, cx + 15, cy + 15,
                             fill="#5b8def", outline="#244b91", width=2)
            self.create_text(cx, cy, text="P", fill="white", font=("Segoe UI", 11, "bold"))


class EventDialog(tk.Toplevel):
    """イベントマスの編集ダイアログ。result は None（キャンセル）/ "delete" / イベント辞書。"""

    def __init__(self, master: tk.Misc, cell: tuple[int, int], event: dict[str, Any] | None,
                 floor_sizes: dict[str, tuple[int, int]], current_floor_id: str) -> None:
        super().__init__(master)
        self.title(f"イベント編集 {cell}")
        self.transient(master)
        self.resizable(False, False)
        self.result: dict[str, Any] | str | None = None
        existing = event is not None
        event = event or {"type": "message", "text": "", "once": False}
        self.kind = tk.StringVar(value=event["type"])
        self.once = tk.BooleanVar(value=event.get("once", False))
        self.gold = tk.StringVar(value=str(event.get("gold", 0)))
        self.item = tk.StringVar(value=event.get("item", ""))
        self.characters = load_character_choices()
        self.character = tk.StringVar(value=self._character_label(event.get("character", "")))
        self.floor_sizes = floor_sizes
        to = event.get("to", {"floorId": current_floor_id, "locate": [cell[0], cell[1]]})
        self.warp_floor = tk.StringVar(value=to["floorId"])
        self.warp_x = tk.StringVar(value=str(to["locate"][0]))
        self.warp_y = tk.StringVar(value=str(to["locate"][1]))
        self.warp_facing = tk.StringVar(value=FACING_LABELS.get(to.get("facing", ""), KEEP_FACING_LABEL))

        body = ttk.Frame(self, padding=12)
        body.pack(fill=tk.BOTH, expand=True)
        kinds = ttk.Frame(body)
        kinds.grid(row=0, column=0, columnspan=2, sticky="w")
        for value, label in EVENT_TYPES.items():
            ttk.Radiobutton(kinds, text=label, value=value, variable=self.kind,
                            command=self._update_state).pack(side=tk.LEFT, padx=(0, 12))
        ttk.Label(body, text="メッセージ（改行可）:").grid(row=1, column=0, columnspan=2, sticky="w", pady=(10, 2))
        self.text = tk.Text(body, width=40, height=5, wrap=tk.CHAR, font=("Yu Gothic UI", 10))
        self.text.grid(row=2, column=0, columnspan=2, sticky="ew")
        self.text.insert("1.0", event.get("text", ""))
        self.once_check = ttk.Checkbutton(body, text="1回だけ表示する", variable=self.once)
        self.once_check.grid(row=3, column=0, columnspan=2, sticky="w", pady=(8, 0))
        ttk.Label(body, text="ゴールド:").grid(row=4, column=0, sticky="w", pady=(8, 0))
        self.gold_entry = ttk.Spinbox(body, from_=0, to=999999, textvariable=self.gold, width=10)
        self.gold_entry.grid(row=4, column=1, sticky="w", pady=(8, 0))
        ttk.Label(body, text="アイテム名:").grid(row=5, column=0, sticky="w", pady=(4, 0))
        self.item_entry = ttk.Entry(body, textvariable=self.item, width=24)
        self.item_entry.grid(row=5, column=1, sticky="w", pady=(4, 0))
        ttk.Label(body, text="宝箱は一度開けると空になります。", foreground="#687078").grid(
            row=6, column=0, columnspan=2, sticky="w", pady=(4, 0))
        ttk.Label(body, text="人物:").grid(row=7, column=0, sticky="w", pady=(8, 0))
        self.character_combo = ttk.Combobox(body, textvariable=self.character, width=22,
                                            values=[self._character_label(cid) for cid, _ in self.characters])
        self.character_combo.grid(row=7, column=1, sticky="w", pady=(8, 0))
        ttk.Label(body, text="人物は assets/data/characters.json で定義します。\n文章を空にすると既定のセリフになります。",
                  foreground="#687078", justify=tk.LEFT).grid(row=8, column=0, columnspan=2, sticky="w", pady=(4, 0))
        ttk.Label(body, text="ワープ先:").grid(row=9, column=0, sticky="w", pady=(8, 0))
        warp = ttk.Frame(body)
        warp.grid(row=9, column=1, sticky="w", pady=(8, 0))
        self.warp_widgets = [
            ttk.Combobox(warp, textvariable=self.warp_floor, values=list(floor_sizes), width=6, state="readonly"),
            ttk.Label(warp, text=" x"),
            ttk.Spinbox(warp, from_=0, to=MAX_MAP_SIZE - 1, textvariable=self.warp_x, width=4),
            ttk.Label(warp, text=" y"),
            ttk.Spinbox(warp, from_=0, to=MAX_MAP_SIZE - 1, textvariable=self.warp_y, width=4),
        ]
        for widget in self.warp_widgets:
            widget.pack(side=tk.LEFT)
        ttk.Label(body, text="到着時の向き:").grid(row=10, column=0, sticky="w", pady=(4, 0))
        self.warp_facing_combo = ttk.Combobox(
            body, textvariable=self.warp_facing, width=8, state="readonly",
            values=[KEEP_FACING_LABEL] + [FACING_LABELS[facing] for facing in FACINGS])
        self.warp_facing_combo.grid(row=10, column=1, sticky="w", pady=(4, 0))
        self.warp_widgets.append(self.warp_facing_combo)
        ttk.Label(body, text="文章を入れるとワープ前に表示します（空なら無言でワープ）。",
                  foreground="#687078").grid(row=11, column=0, columnspan=2, sticky="w", pady=(4, 0))

        buttons = ttk.Frame(body)
        buttons.grid(row=12, column=0, columnspan=2, sticky="e", pady=(12, 0))
        if existing:
            ttk.Button(buttons, text="削除", command=self._delete).pack(side=tk.LEFT, padx=(0, 16))
        ttk.Button(buttons, text="OK", command=self._ok).pack(side=tk.LEFT)
        ttk.Button(buttons, text="キャンセル", command=self.destroy).pack(side=tk.LEFT, padx=(6, 0))

        self._update_state()
        self.geometry(f"+{master.winfo_rootx() + 260}+{master.winfo_rooty() + 80}")
        self.bind("<Escape>", lambda _event: self.destroy())
        self.text.focus_set()
        self.grab_set()
        self.wait_window(self)

    def _character_label(self, character_id: str) -> str:
        for cid, name in self.characters:
            if cid == character_id:
                return f"{cid}: {name}"
        return character_id

    def _update_state(self) -> None:
        kind = self.kind.get()
        self.once_check.configure(state=tk.NORMAL if kind == "message" else tk.DISABLED)
        self.gold_entry.configure(state=tk.NORMAL if kind == "treasure" else tk.DISABLED)
        self.item_entry.configure(state=tk.NORMAL if kind == "treasure" else tk.DISABLED)
        self.character_combo.configure(state="readonly" if kind == "recruit" and self.characters
                                       else tk.NORMAL if kind == "recruit" else tk.DISABLED)
        for widget in self.warp_widgets:
            if isinstance(widget, ttk.Combobox):
                widget.configure(state="readonly" if kind == "warp" else tk.DISABLED)
            elif not isinstance(widget, ttk.Label):
                widget.configure(state=tk.NORMAL if kind == "warp" else tk.DISABLED)

    def _ok(self) -> None:
        text = self.text.get("1.0", "end-1c").rstrip()
        if self.kind.get() == "message":
            if not text:
                messagebox.showerror("入力エラー", "メッセージを入力してください。", parent=self)
                return
            self.result = {"type": "message", "text": text, "once": self.once.get()}
        elif self.kind.get() == "recruit":
            character = self.character.get().split(":", 1)[0].strip()
            if not character:
                messagebox.showerror("入力エラー", "仲間にする人物を選んでください。", parent=self)
                return
            self.result = {"type": "recruit", "text": text, "character": character}
        elif self.kind.get() == "warp":
            try:
                tx, ty = int(self.warp_x.get()), int(self.warp_y.get())
            except ValueError:
                tx = ty = -1
            width, height = self.floor_sizes[self.warp_floor.get()]
            if not (0 <= tx < width and 0 <= ty < height):
                messagebox.showerror(
                    "入力エラー", f"ワープ先は {self.warp_floor.get()} の範囲内（x: 0〜{width - 1}、y: 0〜{height - 1}）で指定してください。",
                    parent=self)
                return
            to: dict[str, Any] = {"floorId": self.warp_floor.get(), "locate": [tx, ty]}
            for facing, label in FACING_LABELS.items():
                if label == self.warp_facing.get():
                    to["facing"] = facing
            self.result = {"type": "warp", "text": text, "to": to}
        else:
            try:
                gold = int(self.gold.get() or 0)
            except ValueError:
                gold = -1
            if gold < 0:
                messagebox.showerror("入力エラー", "ゴールドは0以上の整数で入力してください。", parent=self)
                return
            item = self.item.get().strip()
            if gold == 0 and not item:
                messagebox.showerror("入力エラー", "ゴールドかアイテム名のどちらかを指定してください。", parent=self)
                return
            self.result = {"type": "treasure", "text": text, "gold": gold, "item": item}
        self.destroy()

    def _delete(self) -> None:
        self.result = "delete"
        self.destroy()


class SignDialog(tk.Toplevel):
    """看板の編集ダイアログ。result は None（キャンセル）/ "delete" / 文字列。"""

    def __init__(self, master: tk.Misc, title: str, prompt: str, text: str) -> None:
        super().__init__(master)
        self.title(title)
        self.transient(master)
        self.resizable(False, False)
        self.result: str | None = None
        self.text = tk.StringVar(value=text)

        body = ttk.Frame(self, padding=12)
        body.pack(fill=tk.BOTH, expand=True)
        ttk.Label(body, text=prompt, justify=tk.LEFT).pack(anchor=tk.W)
        entry = ttk.Entry(body, textvariable=self.text, width=36)
        entry.pack(fill=tk.X, pady=(6, 0))
        buttons = ttk.Frame(body)
        buttons.pack(anchor=tk.E, pady=(12, 0))
        if text:
            ttk.Button(buttons, text="削除", command=self._delete).pack(side=tk.LEFT, padx=(0, 16))
        ttk.Button(buttons, text="OK", command=self._ok).pack(side=tk.LEFT)
        ttk.Button(buttons, text="キャンセル", command=self.destroy).pack(side=tk.LEFT, padx=(6, 0))

        self.geometry(f"+{master.winfo_rootx() + 260}+{master.winfo_rooty() + 120}")
        self.bind("<Return>", lambda _event: self._ok())
        self.bind("<Escape>", lambda _event: self.destroy())
        entry.focus_set()
        entry.select_range(0, tk.END)
        self.grab_set()
        self.wait_window(self)

    def _ok(self) -> None:
        text = self.text.get().strip()
        self.result = text if text else "delete"
        self.destroy()

    def _delete(self) -> None:
        self.result = "delete"
        self.destroy()


class MapEditor(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("maze3d Map Editor")
        self.geometry("1100x760")
        self.minsize(800, 560)
        self.map_path: Path | None = None
        self.dirty = False
        self.tool = tk.StringVar(value="wall")
        self.floor_choice = tk.StringVar()
        self.status = tk.StringVar(value="壁／ドアは境界線をドラッグして描きます。")
        self.pending_stair: dict[str, Any] | None = None
        self.pending_warp: dict[str, Any] | None = None
        self.cursor_cell = tk.StringVar()
        self.spawn: dict[str, Any] | None = None
        self.floors: list[dict[str, Any]] = [self._new_floor("1F", 1, 1)]
        self.stairs: list[dict[str, Any]] = []
        self._build_menu()
        self._build_ui()
        self._refresh_floor_choices()
        self.protocol("WM_DELETE_WINDOW", self._close)
        self.bind("<Control-s>", lambda _event: self.save())
        self.bind("<Control-o>", lambda _event: self.open_map())
        self.bind("<Control-Shift-S>", lambda _event: self.save_as())
        self._update_title()

    @staticmethod
    def floor_level(floor_id: str) -> int | None:
        import re
        match = re.fullmatch(r"B(\d+)F?|(-?\d+)F?|G", floor_id.strip().upper())
        if match:
            if floor_id.strip().upper().startswith("B"):
                return -int(match.group(1))
            if match.group(2) is not None:
                return int(match.group(2))
            if floor_id.strip().upper() == "G":
                return 0
        match = re.fullmatch(r"(?:-)?\d+", floor_id.strip())
        return int(match.group(0)) if match else None

    def stair_icon(self, stair: dict[str, Any], floor_id: str) -> str:
        levels = [self.floor_level(end["floorId"]) for end in stair["ends"]]
        if None in levels or levels[0] == levels[1]:
            return "↕"
        current_level = self.floor_level(floor_id)
        if current_level is None:
            return "↕"
        return "▲" if current_level == min(levels) else "▼"

    def switch_floor_by_wheel(self, delta: int) -> None:
        ordered = sorted(self.floors, key=lambda floor: (
            self.floor_level(floor["id"]) is None,
            self.floor_level(floor["id"]) if self.floor_level(floor["id"]) is not None else 0,
            floor["id"]))
        current_id = self.current_floor["id"]
        ids = [floor["id"] for floor in ordered]
        if current_id not in ids:
            return
        index = ids.index(current_id)
        direction = 1 if delta > 0 else -1
        next_index = index + direction
        if not 0 <= next_index < len(ids):
            self.status.set("これ以上階層を移動できません。")
            return
        self.floor_choice.set(ids[next_index])
        self._floor_changed()

    @staticmethod
    def _new_floor(floor_id: str, width: int, height: int) -> dict[str, Any]:
        return {"id": floor_id, "width": width, "height": height, "edges": {}, "events": {}, "signs": {}}

    @property
    def current_floor(self) -> dict[str, Any]:
        return self.floors[self.floor_index]

    @property
    def floor_index(self) -> int:
        selected = self.floor_choice.get()
        for index, floor in enumerate(self.floors):
            if floor["id"] == selected:
                return index
        return 0

    def _build_menu(self) -> None:
        menu = tk.Menu(self)
        file_menu = tk.Menu(menu, tearoff=False)
        file_menu.add_command(label="新規", command=self.new_map, accelerator="Ctrl+N")
        file_menu.add_command(label="開く...", command=self.open_map, accelerator="Ctrl+O")
        file_menu.add_separator()
        file_menu.add_command(label="保存", command=self.save, accelerator="Ctrl+S")
        file_menu.add_command(label="名前を付けて保存...", command=self.save_as, accelerator="Ctrl+Shift+S")
        file_menu.add_separator()
        file_menu.add_command(label="終了", command=self._close)
        menu.add_cascade(label="ファイル", menu=file_menu)
        self.config(menu=menu)
        self.bind("<Control-n>", lambda _event: self.new_map())

    def _build_ui(self) -> None:
        outer = ttk.Frame(self, padding=10)
        outer.pack(fill=tk.BOTH, expand=True)

        sidebar = ttk.Frame(outer, width=235)
        sidebar.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 10))
        sidebar.pack_propagate(False)
        ttk.Label(sidebar, text="フロア", font=("Segoe UI", 11, "bold")).pack(anchor=tk.W)
        self.floor_combo = ttk.Combobox(sidebar, textvariable=self.floor_choice, state="readonly")
        self.floor_combo.pack(fill=tk.X, pady=(6, 5))
        self.floor_combo.bind("<<ComboboxSelected>>", self._floor_changed)
        floor_buttons = ttk.Frame(sidebar)
        floor_buttons.pack(fill=tk.X)
        ttk.Button(floor_buttons, text="追加", command=self.add_floor).pack(side=tk.LEFT, fill=tk.X, expand=True)
        ttk.Button(floor_buttons, text="削除", command=self.delete_floor).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(5, 0))
        ttk.Button(sidebar, text="サイズ変更...", command=self.resize_floor).pack(fill=tk.X, pady=(5, 2))
        ttk.Label(sidebar, text="配置に合わせて範囲が自動拡張されます。", wraplength=220,
              foreground="#687078").pack(anchor=tk.W, pady=(0, 8))

        ttk.Separator(sidebar).pack(fill=tk.X, pady=5)
        ttk.Label(sidebar, text="編集ツール", font=("Segoe UI", 11, "bold")).pack(anchor=tk.W, pady=(5, 3))
        tools = [
            ("wall", "壁（ドラッグ方向）"),
            ("door", "ドア（ドラッグ方向）"),
            ("oneway", "一方通行の壁（クリック）"),
            ("erase", "消去"),
            ("stair", "階段（両端を指定）"),
            ("warp", "ワープ（元→先を指定）"),
            ("spawn", "ゲーム開始地点"),
            ("event", "イベント（メッセージ／宝箱／人間／ワープ）"),
            ("sign", "看板（立ち止まると表示）"),
        ]
        for value, text in tools:
            ttk.Radiobutton(sidebar, text=text, value=value, variable=self.tool,
                            command=self._tool_changed).pack(anchor=tk.W, pady=2)

        ttk.Separator(sidebar).pack(fill=tk.X, pady=10)
        ttk.Label(sidebar, text="凡例", font=("Segoe UI", 10, "bold")).pack(anchor=tk.W)
        ttk.Label(sidebar, text="■ 壁  ■ ドア  ● 階段  ● P 開始  ! イベント", foreground="#495057").pack(anchor=tk.W, pady=4)
        ttk.Label(sidebar, text="壁・ドアは境界線上をドラッグ。\nドラッグ方向に沿って描画します。\nクリック時は近い境界に1線分を配置。\n一方通行の壁は、通る前の側から\n境界の近くをクリック（再クリックで反転）。\n階段は異なるフロアのセルを\n順に2回クリック。開始地点は\n開始地点ツールでセルをクリック。\nイベント（メッセージ／宝箱／人間／\nワープ）はセルをクリックして編集。\n看板はセル内の、見る方向の辺の\n近くをクリック（ダイアログで削除可）。\n\n点線の外側も描画できます。",
                  justify=tk.LEFT, wraplength=220).pack(anchor=tk.W, pady=(6, 0))

        area = ttk.Frame(outer)
        area.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        heading = ttk.Frame(area)
        heading.pack(fill=tk.X, pady=(0, 7))
        self.map_label = ttk.Label(heading, text="", font=("Segoe UI", 11, "bold"))
        self.map_label.pack(side=tk.LEFT)
        ttk.Label(heading, text="左クリック／ドラッグで編集", foreground="#687078").pack(side=tk.RIGHT)
        ttk.Label(heading, textvariable=self.cursor_cell, foreground="#244b91",
                  font=("Segoe UI", 10, "bold")).pack(side=tk.RIGHT, padx=(0, 16))

        canvas_frame = ttk.Frame(area)
        canvas_frame.pack(fill=tk.BOTH, expand=True)
        self.canvas = MapCanvas(canvas_frame, self)
        horizontal = ttk.Scrollbar(canvas_frame, orient=tk.HORIZONTAL, command=self.canvas.xview)
        vertical = ttk.Scrollbar(canvas_frame, orient=tk.VERTICAL, command=self.canvas.yview)
        self.canvas.configure(xscrollcommand=horizontal.set, yscrollcommand=vertical.set)
        self.canvas.grid(row=0, column=0, sticky="nsew")
        vertical.grid(row=0, column=1, sticky="ns")
        horizontal.grid(row=1, column=0, sticky="ew")
        canvas_frame.rowconfigure(0, weight=1)
        canvas_frame.columnconfigure(0, weight=1)

        ttk.Label(self, textvariable=self.status, anchor=tk.W, padding=(10, 5), relief=tk.SUNKEN).pack(fill=tk.X)
        self._update_map_label()

    def _refresh_floor_choices(self) -> None:
        previous = self.floor_choice.get()
        values = [floor["id"] for floor in sorted(self.floors, key=lambda floor: (
            self.floor_level(floor["id"]) is None,
            self.floor_level(floor["id"]) if self.floor_level(floor["id"]) is not None else 0,
            floor["id"]))]
        self.floor_combo.configure(values=values)
        if previous in values:
            self.floor_choice.set(previous)
        else:
            self.floor_choice.set(values[0])
        self._update_map_label()
        self.canvas.redraw()

    def _update_map_label(self) -> None:
        if hasattr(self, "map_label"):
            floor = self.current_floor
            self.map_label.configure(text=f"{floor['id']}  —  {floor['width']} × {floor['height']} (自動拡張)")

    def _floor_changed(self, _event: tk.Event | None = None) -> None:
        self._update_map_label()
        self.canvas.redraw()
        self.status.set(f"{self.current_floor['id']} を編集中です。")

    def _tool_changed(self) -> None:
        if self.tool.get() != "stair" and self.pending_stair:
            self.pending_stair = None
            self.canvas.redraw()
        if self.tool.get() != "warp" and self.pending_warp:
            self.pending_warp = None
            self.canvas.redraw()
        labels = {"wall": "壁", "door": "ドア", "oneway": "一方通行の壁", "erase": "消去",
                  "stair": "階段", "warp": "ワープ", "spawn": "ゲーム開始地点", "event": "イベント", "sign": "看板"}
        if self.tool.get() == "warp":
            self.status.set("ワープ: ワープ元のセルをクリックしてください。")
        else:
            self.status.set(f"{labels[self.tool.get()]}ツールを選択しました。")

    def mark_changed(self) -> None:
        self.dirty = True
        self._update_title()

    def _update_title(self) -> None:
        name = self.map_path.name if self.map_path else "新規マップ"
        self.title(f"{'*' if self.dirty else ''}{name} — maze3d Map Editor")

    def place_stair(self, cell: tuple[int, int]) -> None:
        old_size = (self.current_floor["width"], self.current_floor["height"])
        self.expand_for_cell(cell)
        if self.pending_stair is None and old_size != (
            self.current_floor["width"], self.current_floor["height"]):
            self.mark_changed()
        endpoint = {"floorId": self.current_floor["id"], "locate": [cell[0], cell[1]]}
        if self.pending_stair is None:
            self.pending_stair = endpoint
            self.status.set(f"階段の1端目: {endpoint['floorId']} {cell}。フロアを選び、2端目をクリックしてください。")
        else:
            if (endpoint["floorId"] == self.pending_stair["floorId"]
                    and endpoint["locate"] == self.pending_stair["locate"]):
                self.status.set("同じセルは両端に指定できません。別のセルをクリックしてください。")
                return
            stair_id = self._next_stair_id()
            self.stairs.append({"id": stair_id, "ends": [self.pending_stair, endpoint]})
            self.pending_stair = None
            self.mark_changed()
            self.status.set(f"階段 {stair_id} を双方向で接続しました。")
        self.canvas.redraw()

    def place_warp(self, cell: tuple[int, int]) -> None:
        """1回目のクリックでワープ元、2回目（別フロア可）でワープ先を決めてワープ床を作る。"""
        floor = self.current_floor
        if self.pending_warp is None:
            existing = floor["events"].get(cell)
            if existing and existing["type"] != "warp" and not messagebox.askyesno(
                    "イベントの置き換え",
                    f"{cell} には{EVENT_TYPES[existing['type']]}イベントがあります。ワープに置き換えますか？", parent=self):
                return
            self.expand_for_cell(cell)
            self.pending_warp = {"floorId": floor["id"], "locate": [cell[0], cell[1]]}
            self.status.set(f"ワープ元: {floor['id']} {cell}。必要ならフロアを切り替えて、ワープ先のセルをクリックしてください。")
        else:
            source = self.pending_warp
            if source["floorId"] == floor["id"] and source["locate"] == [cell[0], cell[1]]:
                self.status.set("ワープ元と同じセルは指定できません。別のセルをクリックしてください。")
                return
            self.expand_for_cell(cell)
            source_floor = next(f for f in self.floors if f["id"] == source["floorId"])
            source_cell = (source["locate"][0], source["locate"][1])
            previous = source_floor["events"].get(source_cell)
            # 既存のワープを付け直すときは、文章と到着時の向きを引き継ぐ
            event = {"type": "warp", "text": "", "to": {"floorId": floor["id"], "locate": [cell[0], cell[1]]}}
            if previous and previous["type"] == "warp":
                event["text"] = previous.get("text", "")
                if "facing" in previous["to"]:
                    event["to"]["facing"] = previous["to"]["facing"]
            source_floor["events"][source_cell] = event
            self.pending_warp = None
            self.mark_changed()
            self.status.set(f"ワープを作成しました: {source['floorId']} {source_cell} → {floor['id']} {cell}。"
                            "文章や到着時の向きはイベントツールで編集できます。")
        self.canvas.redraw()

    def place_spawn(self, cell: tuple[int, int]) -> None:
        self.expand_for_cell(cell)
        locate = [cell[0], cell[1]]
        facing = self.spawn.get("facing", "N") if self.spawn else "N"
        if self.spawn and self.spawn["floorId"] == self.current_floor["id"] and self.spawn["locate"] == locate:
            # 同じセルを再クリックすると時計回りに向きを変える
            facing = FACINGS[(FACINGS.index(facing) + 1) % len(FACINGS)]
            message = f"ゲーム開始の向きを {FACING_LABELS[facing]} に変更しました。"
        else:
            message = (f"ゲーム開始地点を {self.current_floor['id']} の {cell}（{FACING_LABELS[facing]}）に設定しました。"
                       "同じセルを再クリックで向きを変更。")
        self.spawn = {"floorId": self.current_floor["id"], "locate": locate, "facing": facing}
        self.mark_changed()
        self.status.set(message)
        self.canvas.redraw()

    def edit_event(self, cell: tuple[int, int]) -> None:
        floor = self.current_floor
        current = floor["events"].get(cell)
        dialog = EventDialog(self, cell, current, {f["id"]: (f["width"], f["height"]) for f in self.floors},
                             floor["id"])
        if dialog.result is None:
            return
        if dialog.result == "delete":
            floor["events"].pop(cell, None)
            self.status.set(f"{floor['id']} {cell} のイベントを削除しました。")
        else:
            self.expand_for_cell(cell)
            floor["events"][cell] = dialog.result
            self.status.set(f"{floor['id']} {cell} に{EVENT_TYPES[dialog.result['type']]}イベントを設定しました。")
        self.mark_changed()
        self.canvas.redraw()

    def edit_sign(self, cell: tuple[int, int], facing: str) -> None:
        floor = self.current_floor
        key = (cell[0], cell[1], facing)
        current = floor["signs"].get(key, "")
        dialog = SignDialog(self, f"看板 {floor['id']} {cell} {FACING_LABELS[facing]}向き",
                            f"{floor['id']} {cell} で{FACING_LABELS[facing]}を向いたときに表示する文字:", current)
        if dialog.result is None or dialog.result == current:
            return
        text = "" if dialog.result == "delete" else dialog.result
        if text:
            self.expand_for_cell(cell)
            floor["signs"][key] = text
            self.status.set(f"{floor['id']} {cell} {FACING_LABELS[facing]}向きに看板「{text}」を設定しました。")
        elif key in floor["signs"]:
            del floor["signs"][key]
            self.status.set(f"{floor['id']} {cell} {FACING_LABELS[facing]}向きの看板を削除しました。")
        else:
            return
        self.mark_changed()
        self.canvas.redraw()

    def expand_for_edge(self, edge: tuple[str, int, int]) -> None:
        axis, x, y = edge
        floor = self.current_floor
        new_width = max(floor["width"], x + 1 if axis == "h" else x)
        new_height = max(floor["height"], y if axis == "h" else y + 1)
        if new_width > MAX_MAP_SIZE or new_height > MAX_MAP_SIZE:
            return
        if (new_width, new_height) != (floor["width"], floor["height"]):
            floor["width"], floor["height"] = new_width, new_height
            self._update_map_label()

    def expand_for_cell(self, cell: tuple[int, int]) -> None:
        floor = self.current_floor
        new_width = max(floor["width"], cell[0] + 1)
        new_height = max(floor["height"], cell[1] + 1)
        if new_width > MAX_MAP_SIZE or new_height > MAX_MAP_SIZE:
            return
        if (new_width, new_height) != (floor["width"], floor["height"]):
            floor["width"], floor["height"] = new_width, new_height
            self._update_map_label()

    def _next_stair_id(self) -> str:
        used = {stair["id"] for stair in self.stairs}
        number = 1
        while f"stair_{number}" in used:
            number += 1
        return f"stair_{number}"

    def erase_near(self, px: float, py: float, gx: float, gy: float) -> None:
        floor = self.current_floor
        # 看板の札は境界のすぐ内側にあるので、境界線より先に判定する
        for key in list(floor["signs"]):
            sign_x, sign_y, facing = key
            dx, dy = FACING_VECTORS[facing]
            tag_x = GRID_ORIGIN + (sign_x + 0.5 + dx * 0.3) * CELL_SIZE
            tag_y = GRID_ORIGIN + (sign_y + 0.5 + dy * 0.3) * CELL_SIZE
            if abs(px - tag_x) <= 14 and abs(py - tag_y) <= 9:
                text = floor["signs"].pop(key)
                self.mark_changed()
                self.status.set(f"看板「{text}」を削除しました。")
                return

        nearest: tuple[float, tuple[str, int, int]] | None = None
        for edge in floor["edges"]:
            axis, ex, ey = edge
            if axis == "h":
                x1, y1 = GRID_ORIGIN + ex * CELL_SIZE, GRID_ORIGIN + ey * CELL_SIZE
                x2, y2 = x1 + CELL_SIZE, y1
                distance = abs(py - y1) if x1 - 8 <= px <= x2 + 8 else min(
                    ((px - x1) ** 2 + (py - y1) ** 2) ** 0.5,
                    ((px - x2) ** 2 + (py - y2) ** 2) ** 0.5)
            else:
                x1, y1 = GRID_ORIGIN + ex * CELL_SIZE, GRID_ORIGIN + ey * CELL_SIZE
                x2, y2 = x1, y1 + CELL_SIZE
                distance = abs(px - x1) if y1 - 8 <= py <= y2 + 8 else min(
                    ((px - x1) ** 2 + (py - y1) ** 2) ** 0.5,
                    ((px - x2) ** 2 + (py - y2) ** 2) ** 0.5)
            if nearest is None or distance < nearest[0]:
                nearest = (distance, edge)

        if nearest and nearest[0] <= 13:
            del floor["edges"][nearest[1]]
            self.mark_changed()
            return

        cell_x, cell_y = int(gx), int(gy)
        if (self.spawn and self.spawn["floorId"] == floor["id"]
                and self.spawn["locate"] == [cell_x, cell_y]):
            self.spawn = None
            self.mark_changed()
            self.status.set("ゲーム開始地点を解除しました。")
            return
        if (cell_x, cell_y) in floor["events"]:
            del floor["events"][(cell_x, cell_y)]
            self.mark_changed()
            self.status.set("イベントを削除しました。")
            return
        cell_signs = [key for key in floor["signs"] if key[:2] == (cell_x, cell_y)]
        if cell_signs:
            for key in cell_signs:
                del floor["signs"][key]
            self.mark_changed()
            self.status.set(f"看板 {len(cell_signs)} 件を削除しました。")
            return
        for index in range(len(self.stairs) - 1, -1, -1):
            stair = self.stairs[index]
            if any(end["floorId"] == floor["id"] and end["locate"] == [cell_x, cell_y]
                   for end in stair["ends"]):
                del self.stairs[index]
                self.mark_changed()
                self.status.set(f"階段 {stair['id']} を削除しました（接続も削除）。")
                return

    def add_floor(self) -> None:
        floor_id = simpledialog.askstring("フロア追加", "フロア名（例: 2F, B1F）:", parent=self)
        if floor_id is None:
            return
        floor_id = floor_id.strip()
        if not floor_id:
            messagebox.showerror("入力エラー", "フロア名を入力してください。", parent=self)
            return
        if any(floor["id"] == floor_id for floor in self.floors):
            messagebox.showerror("入力エラー", "そのフロア名は既に使われています。", parent=self)
            return
        self.floors.append(self._new_floor(floor_id, 1, 1))
        self.floor_choice.set(floor_id)
        self._refresh_floor_choices()
        self.mark_changed()
        self.status.set(f"フロア {floor_id} を追加しました。")

    def delete_floor(self) -> None:
        if len(self.floors) <= 1:
            messagebox.showwarning("削除できません", "マップには最低1フロア必要です。", parent=self)
            return
        floor = self.current_floor
        linked = [stair for stair in self.stairs
                  if any(end["floorId"] == floor["id"] for end in stair["ends"])]
        warps = self._warps_into(floor["id"], lambda x, y: True)
        warps = [(other, cell) for other, cell in warps if other is not floor]
        prompt = f"フロア {floor['id']} を削除しますか？"
        if linked:
            prompt += f"\nこのフロアに接続する階段 {len(linked)} 件も削除されます。"
        if warps:
            prompt += f"\nこのフロアへのワープ {len(warps)} 件も削除されます。"
        if not messagebox.askyesno("フロア削除", prompt, parent=self):
            return
        for other, cell in warps:
            del other["events"][cell]
        self.floors.remove(floor)
        self.stairs = [stair for stair in self.stairs
                       if not any(end["floorId"] == floor["id"] for end in stair["ends"])]
        if self.spawn and self.spawn["floorId"] == floor["id"]:
            self.spawn = None
        self.pending_stair = None
        self.pending_warp = None
        self.floor_choice.set(self.floors[0]["id"])
        self._refresh_floor_choices()
        self.mark_changed()
        self.status.set(f"フロア {floor['id']} を削除しました。")

    def resize_floor(self) -> None:
        floor = self.current_floor
        width = simpledialog.askinteger("サイズ変更", "幅（セル数）:", initialvalue=floor["width"],
                                        minvalue=MIN_MAP_SIZE, maxvalue=MAX_MAP_SIZE, parent=self)
        if width is None:
            return
        height = simpledialog.askinteger("サイズ変更", "高さ（セル数）:", initialvalue=floor["height"],
                                         minvalue=MIN_MAP_SIZE, maxvalue=MAX_MAP_SIZE, parent=self)
        if height is None:
            return
        out_edges = [edge for edge in floor["edges"] if not self._edge_in_bounds(edge, width, height)]
        out_stairs = [stair for stair in self.stairs
                      if any(end["floorId"] == floor["id"] and
                             (end["locate"][0] >= width or end["locate"][1] >= height)
                             for end in stair["ends"])]
        out_spawn = bool(self.spawn and self.spawn["floorId"] == floor["id"]
                 and (self.spawn["locate"][0] >= width or self.spawn["locate"][1] >= height))
        out_events = [cell for cell in floor["events"] if cell[0] >= width or cell[1] >= height]
        out_signs = [key for key in floor["signs"] if key[0] >= width or key[1] >= height]
        out_warps = [(other, cell) for other, cell in
                     self._warps_into(floor["id"], lambda x, y: x >= width or y >= height)
                     if not (other is floor and cell in out_events)]
        if (out_edges or out_stairs or out_spawn or out_events or out_signs or out_warps) and not messagebox.askyesno(
            "範囲外データ", f"範囲外になる線分 {len(out_edges)} 本、階段 {len(out_stairs)} 件、イベント {len(out_events)} 件、看板 {len(out_signs)} 件、開始地点 {'1件' if out_spawn else '0件'}、ここへのワープ {len(out_warps)} 件を削除して変更しますか？",
                parent=self):
            return
        for edge in out_edges:
            del floor["edges"][edge]
        if out_stairs:
            removed = {stair["id"] for stair in out_stairs}
            self.stairs = [stair for stair in self.stairs if stair["id"] not in removed]
        if out_spawn:
            self.spawn = None
        for cell in out_events:
            del floor["events"][cell]
        for key in out_signs:
            del floor["signs"][key]
        for other, cell in out_warps:
            del other["events"][cell]
        floor["width"], floor["height"] = width, height
        self._update_map_label()
        self.canvas.redraw()
        self.mark_changed()
        self.status.set(f"{floor['id']} のサイズを {width} × {height} に変更しました。")

    def _warps_into(self, floor_id: str, hit: Any) -> list[tuple[dict[str, Any], tuple[int, int]]]:
        """floor_id 行きで、行き先座標 (x, y) が hit(x, y) を満たすワープの (フロア, セル) 一覧。"""
        return [(floor, cell) for floor in self.floors for cell, event in floor["events"].items()
                if event["type"] == "warp" and event["to"]["floorId"] == floor_id
                and hit(*event["to"]["locate"])]

    @staticmethod
    def _edge_in_bounds(edge: tuple[str, int, int], width: int, height: int) -> bool:
        axis, x, y = edge
        if axis == "h":
            return 0 <= x < width and 0 <= y <= height
        return 0 <= x <= width and 0 <= y < height

    def new_map(self) -> None:
        if not self._confirm_discard():
            return
        self.floors = [self._new_floor("1F", 1, 1)]
        self.stairs = []
        self.spawn = None
        self.pending_stair = None
        self.pending_warp = None
        self.map_path = None
        self.dirty = False
        self.floor_choice.set("1F")
        self._refresh_floor_choices()
        self.status.set("新しいマップを作成しました。")
        self._update_title()

    def _confirm_discard(self) -> bool:
        if not self.dirty:
            return True
        answer = messagebox.askyesnocancel("未保存の変更", "変更を保存しますか？", parent=self)
        if answer is None:
            return False
        if answer:
            return self.save()
        return True

    def open_map(self) -> None:
        if not self._confirm_discard():
            return
        filename = filedialog.askopenfilename(
            parent=self, title="マップを開く", filetypes=[("JSON マップ", "*.json"), ("すべてのファイル", "*.*")])
        if not filename:
            return
        try:
            floors, stairs, spawn, overlaps = self._load_document(Path(filename))
        except (OSError, json.JSONDecodeError, ValueError, TypeError) as error:
            messagebox.showerror("読み込みエラー", str(error), parent=self)
            return
        self.floors, self.stairs, self.spawn = floors, stairs, spawn
        self.pending_stair = None
        self.pending_warp = None
        self.map_path = Path(filename)
        self.dirty = False
        self.floor_choice.set(floors[0]["id"])
        self._refresh_floor_choices()
        if overlaps:
            details = "\n".join(overlaps[:8])
            if len(overlaps) > 8:
                details += f"\nほか {len(overlaps) - 8} 件"
            self.status.set(f"警告: 重複した線分 {len(overlaps)} 件。後方の定義を優先しました。")
            messagebox.showwarning("重複する壁・ドア", f"重複した線分が {len(overlaps)} 件あります。後方定義を優先しました。\n\n{details}", parent=self)
        else:
            self.status.set(f"{Path(filename).name} を読み込みました。")
        self._update_title()

    def _load_document(self, path: Path) -> tuple[
            list[dict[str, Any]], list[dict[str, Any]], dict[str, Any] | None, list[str]]:
        document = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(document, dict) or document.get("version") != SCHEMA_VERSION:
            raise ValueError(f"対応するJSON形式ではありません（version: {SCHEMA_VERSION} が必要です）。")
        raw_floors = document.get("floors")
        if not isinstance(raw_floors, list) or not raw_floors:
            raise ValueError("floors は1件以上の配列で指定してください。")

        floors: list[dict[str, Any]] = []
        floor_ids: set[str] = set()
        overlaps: list[str] = []
        for floor_number, raw_floor in enumerate(raw_floors, start=1):
            if not isinstance(raw_floor, dict):
                raise ValueError(f"floors[{floor_number - 1}] はオブジェクトではありません。")
            floor_id = raw_floor.get("id")
            if not isinstance(floor_id, str) or not floor_id.strip() or floor_id in floor_ids:
                raise ValueError(f"floors[{floor_number - 1}].id が空、または重複しています。")
            floor_ids.add(floor_id)
            width = validate_size(raw_floor.get("width"), f"{floor_id}.width")
            height = validate_size(raw_floor.get("height"), f"{floor_id}.height")
            floor = self._new_floor(floor_id, width, height)
            raw_edges = raw_floor.get("edges", [])
            if not isinstance(raw_edges, list):
                raise ValueError(f"{floor_id}.edges は配列で指定してください。")
            for edge_number, raw_edge in enumerate(raw_edges, start=1):
                if not isinstance(raw_edge, dict):
                    raise ValueError(f"{floor_id}.edges[{edge_number - 1}] はオブジェクトではありません。")
                axis = raw_edge.get("axis")
                kind = raw_edge.get("type")
                if axis not in ("h", "v") or kind not in EDGE_TYPES:
                    raise ValueError(f"{floor_id}.edges[{edge_number - 1}] の axis/type が不正です。")
                if kind == "oneway":
                    passing = raw_edge.get("pass")
                    if passing not in (("N", "S") if axis == "h" else ("E", "W")):
                        raise ValueError(f"{floor_id}.edges[{edge_number - 1}].pass は "
                                         f"{'N / S' if axis == 'h' else 'E / W'} で指定してください。")
                    kind = ONEWAY_PREFIX + passing
                x, y = parse_coordinate(raw_edge.get("locate"), f"{floor_id}.edges[{edge_number - 1}]")
                length = raw_edge.get("length", 1)
                if isinstance(length, bool) or not isinstance(length, int) or length < 1:
                    raise ValueError(f"{floor_id}.edges[{edge_number - 1}].length は1以上の整数です。")
                for offset in range(length):
                    edge = (axis, x + offset, y) if axis == "h" else (axis, x, y + offset)
                    if not self._edge_in_bounds(edge, width, height):
                        raise ValueError(f"{floor_id}.edges[{edge_number - 1}] がフロア範囲外です。")
                    if edge in floor["edges"]:
                        previous = floor["edges"][edge]
                        overlaps.append(f"{floor_id} {axis} {edge[1:]}: {previous} → {kind}（定義 {edge_number}）")
                    floor["edges"][edge] = kind
            raw_events = raw_floor.get("events", [])
            if not isinstance(raw_events, list):
                raise ValueError(f"{floor_id}.events は配列で指定してください。")
            for event_number, raw_event in enumerate(raw_events):
                label = f"{floor_id}.events[{event_number}]"
                event = parse_event(raw_event, label)
                ex, ey = parse_coordinate(raw_event.get("locate"), label)
                if not 0 <= ex < width or not 0 <= ey < height:
                    raise ValueError(f"{label} がセル範囲外です。")
                floor["events"][(ex, ey)] = event
            raw_signs = raw_floor.get("signs", [])
            if not isinstance(raw_signs, list):
                raise ValueError(f"{floor_id}.signs は配列で指定してください。")
            for sign_number, raw_sign in enumerate(raw_signs):
                label = f"{floor_id}.signs[{sign_number}]"
                if not isinstance(raw_sign, dict):
                    raise ValueError(f"{label} はオブジェクトではありません。")
                sx, sy = parse_coordinate(raw_sign.get("locate"), label)
                if not 0 <= sx < width or not 0 <= sy < height:
                    raise ValueError(f"{label} がセル範囲外です。")
                facing = raw_sign.get("facing")
                if facing not in FACINGS:
                    raise ValueError(f"{label}.facing は N / E / S / W のいずれかで指定してください。")
                text = raw_sign.get("text")
                if not isinstance(text, str) or not text.strip():
                    raise ValueError(f"{label}.text に文字を指定してください。")
                floor["signs"][(sx, sy, facing)] = text
            floors.append(floor)

        floor_sizes = {floor["id"]: (floor["width"], floor["height"]) for floor in floors}
        for floor in floors:
            for cell, event in floor["events"].items():
                if event["type"] != "warp":
                    continue
                to = event["to"]
                size = floor_sizes.get(to["floorId"])
                if size is None:
                    raise ValueError(f"{floor['id']} {cell} のワープ先フロア {to['floorId']} がありません。")
                if not (0 <= to["locate"][0] < size[0] and 0 <= to["locate"][1] < size[1]):
                    raise ValueError(f"{floor['id']} {cell} のワープ先 {to['locate']} がセル範囲外です。")

        raw_stairs = document.get("stairs", [])
        if not isinstance(raw_stairs, list):
            raise ValueError("stairs は配列で指定してください。")
        stairs: list[dict[str, Any]] = []
        stair_ids: set[str] = set()
        floor_by_id = {floor["id"]: floor for floor in floors}
        for index, raw_stair in enumerate(raw_stairs):
            if not isinstance(raw_stair, dict):
                raise ValueError(f"stairs[{index}] はオブジェクトではありません。")
            stair_id = raw_stair.get("id")
            ends = raw_stair.get("ends")
            if not isinstance(stair_id, str) or not stair_id or stair_id in stair_ids:
                raise ValueError(f"stairs[{index}].id が空、または重複しています。")
            if not isinstance(ends, list) or len(ends) != 2:
                raise ValueError(f"階段 {stair_id} は ends に2地点を指定してください。")
            parsed_ends = []
            for end_index, end in enumerate(ends):
                if not isinstance(end, dict) or end.get("floorId") not in floor_by_id:
                    raise ValueError(f"階段 {stair_id} の端点 {end_index + 1} の floorId が不正です。")
                floor_id = end["floorId"]
                x, y = parse_coordinate(end.get("locate"), f"階段 {stair_id} の端点 {end_index + 1}")
                floor = floor_by_id[floor_id]
                if not 0 <= x < floor["width"] or not 0 <= y < floor["height"]:
                    raise ValueError(f"階段 {stair_id} の端点 {end_index + 1} がセル範囲外です。")
                parsed_ends.append({"floorId": floor_id, "locate": [x, y]})
            if parsed_ends[0] == parsed_ends[1]:
                raise ValueError(f"階段 {stair_id} の両端が同じセルです。")
            stair_ids.add(stair_id)
            stairs.append({"id": stair_id, "ends": parsed_ends})
        raw_spawn = document.get("spawn")
        spawn = None
        if raw_spawn is not None:
            if not isinstance(raw_spawn, dict) or raw_spawn.get("floorId") not in floor_by_id:
                raise ValueError("spawn.floorId が不正です。")
            spawn_floor_id = raw_spawn["floorId"]
            spawn_x, spawn_y = parse_coordinate(raw_spawn.get("locate"), "spawn")
            spawn_floor = floor_by_id[spawn_floor_id]
            if not 0 <= spawn_x < spawn_floor["width"] or not 0 <= spawn_y < spawn_floor["height"]:
                raise ValueError("spawn.locate がセル範囲外です。")
            spawn_facing = raw_spawn.get("facing", "N")
            if spawn_facing not in FACINGS:
                raise ValueError("spawn.facing は N / E / S / W のいずれかで指定してください。")
            spawn = {"floorId": spawn_floor_id, "locate": [spawn_x, spawn_y], "facing": spawn_facing}
        return floors, stairs, spawn, overlaps

    def save(self) -> bool:
        if self.map_path is None:
            return self.save_as()
        try:
            self.map_path.write_text(json.dumps(self._document(), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        except OSError as error:
            messagebox.showerror("保存エラー", str(error), parent=self)
            return False
        self.dirty = False
        self._update_title()
        self.status.set(f"{self.map_path.name} を保存しました。")
        return True

    def save_as(self) -> bool:
        filename = filedialog.asksaveasfilename(
            parent=self, title="マップを保存", defaultextension=".json",
            filetypes=[("JSON マップ", "*.json"), ("すべてのファイル", "*.*")])
        if not filename:
            return False
        self.map_path = Path(filename)
        return self.save()

    def _document(self) -> dict[str, Any]:
        floors = []
        for floor in self.floors:
            floors.append({
                "id": floor["id"],
                "width": floor["width"],
                "height": floor["height"],
                "edges": self._compress_edges(floor["edges"]),
                "events": [{"locate": [x, y], **event} for (x, y), event in
                           sorted(floor["events"].items(), key=lambda item: (item[0][1], item[0][0]))],
            })
            if floor["signs"]:
                floors[-1]["signs"] = [
                    {"locate": [x, y], "facing": facing, "text": text} for (x, y, facing), text in
                    sorted(floor["signs"].items(), key=lambda item: (item[0][1], item[0][0], FACINGS.index(item[0][2])))]
        document = {
            "version": SCHEMA_VERSION,
            "floors": floors,
            "stairs": self.stairs,
        }
        if self.spawn:
            document["spawn"] = self.spawn
        return document

    @staticmethod
    def _compress_edges(edges: dict[tuple[str, int, int], str]) -> list[dict[str, Any]]:
        groups: dict[tuple[str, int, str], list[int]] = {}
        for (axis, x, y), kind in edges.items():
            fixed, start = (y, x) if axis == "h" else (x, y)
            groups.setdefault((axis, fixed, kind), []).append(start)
        output = []
        for axis, fixed, kind in sorted(groups):
            positions = sorted(set(groups[(axis, fixed, kind)]))
            run_start = previous = positions[0]
            for position in positions[1:] + [None]:
                if position is not None and position == previous + 1:
                    previous = position
                    continue
                start_x, start_y = (run_start, fixed) if axis == "h" else (fixed, run_start)
                entry = {"axis": axis, "locate": [start_x, start_y], "type": kind,
                         "length": previous - run_start + 1}
                if kind.startswith(ONEWAY_PREFIX):
                    entry["type"] = "oneway"
                    entry["pass"] = kind[len(ONEWAY_PREFIX):]
                output.append(entry)
                if position is not None:
                    run_start = previous = position
        return output

    def _close(self) -> None:
        if self._confirm_discard():
            self.destroy()


def main() -> None:
    app = MapEditor()
    app.mainloop()


if __name__ == "__main__":
    main()
