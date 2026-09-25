# Canvas Architecture

> Status: **Final baseline — not yet implemented.** The `studyapp_canvas` target exists
> (module anchor only); the engine described here is built in Phase 4 and Phase 6.

The `canvas` module is the interactive engine between the document model and the
renderer. It is plain C++ (no Qt, no GL) and fully testable headless: tests can feed it
synthetic pointer events and inspect the resulting patches and render frames.

```mermaid
flowchart LR
    qt["ui::CanvasWidget<br/>(Qt events)"] --> adapter["ui::CanvasInputAdapter<br/>(normalisation)"]
    adapter -->|"canvas::PointerEvent / WheelEvent /<br/>GestureEvent / KeyEvent"| ctrl[CanvasController]
    ctrl --> tool["active Tool<br/>(state machine)"]
    tool -->|queries| scene[CanvasScene]
    tool -->|preview state| overlay[Preview & overlays]
    tool -->|commit Patch| editor["document::Editor"]
    editor --> doc[PageDocument]
    doc -->|changed(Patch)| scene
    ctrl -->|buildFrame| frame["render::RenderFrame"]
    scene --> frame
    overlay --> frame
    frame --> renderer["render::Renderer"]
```

## 0. Input ownership

The canvas engine **never depends on Qt event types**. `QMouseEvent`, `QTabletEvent`,
`QWheelEvent`, `QKeyEvent`, `QNativeGestureEvent` and friends stop at the `ui` layer:

```
Qt event → ui::CanvasWidget → ui::CanvasInputAdapter (normalisation)
         → canvas::PointerEvent / WheelEvent / GestureEvent / KeyEvent
         → CanvasController → active Tool → document::Editor → Patch
```

* **`ui` owns translation.** `CanvasInputAdapter` is the only code that sees both Qt
  events and canvas events. It maps device kinds, pressure, tilt, coalesced samples,
  timestamps, modifiers and DPI into the plain value types of §4.
* **Platform differences** (Windows Ink vs Wintab, macOS tablet proximity, Wayland tablet
  protocol) are normalised by helpers in `platform/os/*` that the adapter calls, *before*
  the event reaches the canvas.
* **Canvas code stays plain C++ and headless-testable**: tests construct events directly.
* The same rule applies in the other direction: the canvas asks for a cursor via a plain
  `CursorShape` enum and requests repaints via a callback; `ui` maps these to Qt.

## 0.1 Canvas look

The canvas follows the design tokens of ARCHITECTURE.md §3.3: neutral paper-like
background (`canvas`), a subtle dot or line grid (`canvasGrid`), high-contrast content and
minimal chrome — no coloured backgrounds, gradients or glowing grids. The Phase 2
`ui::CanvasPlaceholder` already paints this background; the canvas engine will take the
same token values (they are `core::Color`, usable without Qt).

## 1. Coordinate systems

| Space | Type | Unit | Used for |
|---|---|---|---|
| **Local** | `Vec2` (float) | world units, relative to element origin | Stroke points, shape vertices, text box size |
| **World** | `DVec2` (double) | 1 unit = 1 logical pixel at 100 % zoom (96 units per inch) | Element positions, page bounds, spatial index |
| **View** | `Vec2` | logical (device-independent) pixels, origin top-left of the widget | Input, hit tolerances, overlays, UI |
| **Device** | `Vec2` | physical pixels = view × devicePixelRatio | GL viewport, texture resolution decisions |

Y points down in all spaces (matches Qt and typical page layout). A bounded A4 page is
794 × 1123 world units (210 × 297 mm at 96 units/inch).

Transform chain: `local --(element Transform)--> world --(Camera)--> view --(dpr)--> device`.

### Precision ("floating origin")

World coordinates are doubles so the canvas is effectively infinite. GPUs work in float,
so the renderer never receives raw world coordinates: each draw item's transform is
computed **relative to the camera centre** in double precision on the CPU, then converted
to float. Combined with element-local point storage, strokes stay precise even millions
of units from the origin.

## 2. Camera

```cpp
namespace studyapp::canvas {
class Camera {
public:
    core::DVec2 center() const;         // world point at the viewport centre
    double zoom() const;                // view pixels per world unit; clamped [0.02, 64]
    core::Vec2 viewportSize() const;    // logical pixels
    float devicePixelRatio() const;

    core::Vec2  worldToView(core::DVec2) const;
    core::DVec2 viewToWorld(core::Vec2) const;
    core::DRect visibleWorldRect() const;

    void panBy(core::Vec2 viewDelta);
    void zoomAt(core::Vec2 viewAnchor, double factor);   // keeps the anchor fixed
    void fitRect(core::DRect world, float marginPx);
    void setViewport(core::Vec2 size, float dpr);
};
}
```

* Camera is a value type; each displayed page keeps its last camera (persisted per page
  in workspace settings so pages reopen where the user left them).
* Bounded pages clamp panning so the page cannot be scrolled fully out of view.
* Rotation of the view (tablet users rotating the canvas) is a possible later addition;
  the math is kept in one class so it can be added there.
* Smooth zoom/pan animations interpolate camera values in the UI layer.

## 3. CanvasScene

`CanvasScene` mirrors the displayed `PageDocument` with structures optimised for
interaction and drawing. It is updated incrementally from `PageDocument::changed(Patch)`.

```cpp
class CanvasScene {
public:
    explicit CanvasScene(const document::PageDocument&);
    void onPatch(const document::Patch&);          // incremental update

    void query(core::DRect worldRect, std::vector<ElementId>& out) const; // broad phase
    std::optional<ElementId> topmostAt(core::DVec2, double tolerance) const;
    core::DRect worldBounds(ElementId) const;

    RenderCacheEntry& renderCache(ElementId);      // tessellated mesh, GPU handle, version
    std::span<const DrawOrderEntry> drawOrder() const; // (layer order, z) sorted
};
```

### 3.1 Spatial index

A **sparse spatial hash grid** over world AABBs:

* Cell size ≈ 512 world units; cells in an `unordered_map<CellKey, SmallVector<ElementId>>`
  — sparse, so infinite canvases cost nothing where empty.
* Elements spanning more than 16 cells are kept in a separate "large" list checked on
  every query (few elements are that big).
* Insert/remove/update are O(cells touched). Queries return candidates; callers do exact
  tests.

Chosen for simplicity and good behaviour with the typical distribution (many small,
dense strokes). It sits behind the `CanvasScene` API, so an R-tree or quadtree can replace
it if profiling shows skewed distributions hurting. Benchmarks for 10k/100k elements
are part of Phase 4.

### 3.2 Draw order

A cached array sorted by `(layer.order, element.z)`, rebuilt incrementally on reorder
patches (bulk rebuild when many elements change). Culling = spatial query → mark visible →
walk draw order emitting only visible items, preserving correct painter's order.

### 3.3 Render cache

Per element: tessellated CPU mesh (`render::MeshData`, local space), GPU handle, the
payload pointer it was built from, and the LOD level. Invalidation is by **payload
identity**: a move only changes the transform (no re-tessellation); a restyle or edit
creates a new payload pointer and the entry is rebuilt. CPU meshes are kept so GPU
resources can be recreated after context loss.

## 4. Input model

`ui::CanvasInputAdapter` converts Qt events into these engine events (see §0); tools never
see Qt types.

```cpp
enum class PointerDevice { Mouse, Pen, Eraser /* pen's eraser end */, Touch };
enum class PointerPhase  { Down, Move, Up, Cancel, Hover };

struct PointerEvent {
    PointerPhase phase; PointerDevice device;
    core::Vec2 viewPos;                 // logical px, sub-pixel precision
    float pressure = 1.f;               // 0..1 (mouse = 1)
    core::Vec2 tilt{};                  // degrees, if available
    Buttons buttons; Modifiers modifiers;
    std::uint64_t timestampUs;
    std::span<const PointerSample> coalesced;   // high-rate samples since last event
};
struct WheelEvent { core::Vec2 viewPos; core::Vec2 angleDelta, pixelDelta; Modifiers mods; };
struct GestureEvent { GestureKind kind; core::Vec2 viewPos; double scale; core::Vec2 delta; };
```

* Pen samples arrive at 100–1000 Hz; all samples are processed, but frames are only
  requested once per display refresh (render-on-demand, coalesced `update()`).
* Palm rejection / touch policy: when a pen is in proximity, touch is used only for
  pan/zoom gestures (configurable).
* Platform quirks (Windows Ink vs Wintab, macOS pressure curves, Wayland tablet protocol)
  are normalised in `platform/os/*` before events reach the canvas.

## 5. Tools

Each tool is a small state machine implementing one interface — a legitimate polymorphic
boundary, since tools are selected at run time:

```cpp
class Tool {
public:
    virtual ~Tool() = default;
    virtual void onPointer(const PointerEvent&, ToolContext&) = 0;
    virtual void onKey(const KeyEvent&, ToolContext&) {}
    virtual void onDeactivate(ToolContext&) {}         // cancel previews
    virtual void buildOverlay(OverlayBuilder&, const ToolContext&) const {}
    virtual CursorShape cursor(const ToolContext&) const = 0;
};

struct ToolContext {                  // everything a tool may touch — nothing else
    const Camera& camera;
    const CanvasScene& scene;
    const document::PageDocument& document;
    document::Editor& editor;         // the only way to change the document
    Selection& selection;
    Preview& preview;                 // transient, non-document render state
    const ToolSettings& settings;     // colour, width, brush, shape kind…
    core::IdGenerator& ids;
    TextLayout& textLayout;           // interface; implemented by platform (Qt)
    void requestRedraw();
};
```

Planned tools: Pan, Zoom, Pen, Highlighter, Eraser (whole-stroke and partial),
Select (click / rectangle / lasso), Transform (handles: move, scale, rotate), Shape,
Text, Connector, Image placement, Laser pointer (non-persistent). Temporary tool
switching (hold Space to pan, pen eraser end → Eraser) is handled by `CanvasController`.

### 5.1 Stroke input pipeline (Pen tool)

```
raw samples → dedupe (< 0.5 px moves) → smoothing (One-Euro filter, tunable)
            → pressure curve (per brush) → live preview (incremental tessellation of the tail)
pen up      → simplification (Ramer–Douglas–Peucker, ε ≈ 0.25 view px, pressure-aware)
            → convert to element-local coordinates → commands::createElements → commit
```

The live stroke is rendered from the `Preview` with only its tail re-tessellated per
event, so cost per event is O(new samples), not O(stroke length). Prediction (drawing a
few ms ahead) is a later latency optimisation.

### 5.2 Eraser

* **Stroke eraser**: removes any stroke whose geometry the eraser path touches.
* **Partial eraser**: splits strokes where the eraser circle covers points, producing a
  patch that deletes the original and creates the surviving pieces (new ids).
* The eraser accumulates affected elements during the gesture (preview hides them) and
  commits a single patch on pen-up → one undo step.

## 6. Hit testing

Two-phase, always in world space with tolerance given in **view pixels** (so it feels the
same at every zoom): `toleranceWorld = tolerancePx / camera.zoom()`.

1. **Broad phase**: spatial grid query with the tolerance-inflated point/rect/polygon.
2. **Narrow phase** per candidate, after transforming the query into element-local space
   (inverse transform):
   * Stroke: distance to polyline segments ≤ halfWidth(pressure) + tolerance
     (segment-level AABB early-outs for long strokes).
   * Shape: exact geometry (inside fill, or distance to outline if unfilled).
   * TextBox / Image: local rectangle.
   * Connector: distance to its resolved path.
3. Candidates are tested in **reverse draw order**; hidden/locked layers are skipped per
   the tool's policy.

Rectangle selection: fully-contained vs intersecting (modifier). Lasso: polygon
containment of element geometry (sampled points for strokes).

## 7. Selection and transforms

* `Selection` = set of `ElementId`s + cached combined world bounds; UI-only state, not
  persisted, cleared on page switch, pruned when a patch deletes selected elements.
* Transform handles are drawn in **view space** (constant pixel size) as overlays.
* During a drag the canvas applies a preview transform to the selected elements' draw
  items; on release it commits `commands::transformElements` (and connector updates) as
  one patch.
* Snapping (grid, other elements' edges/centres, angle increments) is a later addition,
  implemented as a pure function over candidate geometry.

## 8. Layers and z-order

Draw order: layers by `order`, elements within a layer by `z`. Reordering commands
compute new fractional keys (e.g. "bring to front" = key after the current max), so a
reorder touches only the moved elements. Hidden layers are skipped in both culling and
hit testing; locked layers are drawn but not hit-testable by editing tools.

## 9. Text on the canvas

* **Display**: text boxes are laid out by `TextLayout` (Qt `QTextLayout` in `platform`)
  and rasterised to textures at a zoom-bucketed resolution (e.g. powers of √2); while
  zooming, the previous texture is stretched until the new raster arrives from a worker.
* **Editing (v1)**: a Qt text editor widget is overlaid on the text box while editing,
  scaled to the current zoom (rotation is temporarily reset while editing). On commit the
  UI converts the Qt document to `document::RichText` and commits `commands::editText`.
  This is the pragmatic approach used by several canvas apps; a fully in-canvas editor is a
  possible later upgrade, isolated behind the Text tool.

## 10. Documents and images

* Images: decoded off-thread by `ImageDecoder` (Qt), uploaded as mip-mapped textures;
  very large images are downscaled to a level appropriate for the current zoom.
* PDF backgrounds: `DocumentRasterizer` (QtPdf in `platform`) renders **tiles** of a PDF
  page at the zoom level needed, on worker threads; tiles are cached (LRU in memory, and
  on disk in the OS cache directory). A low-resolution full-page raster is shown while
  tiles load. Annotations are ordinary elements on layers above the background, so the
  original PDF is never modified; flattening happens only at export.

## 11. Scale: thousands of elements

| Operation | Cost |
|---|---|
| Pan/zoom frame | O(visible) culling via grid + O(draw items) submission |
| Hit test at a point | O(candidates in one cell) |
| Move N elements | O(N) header changes; no re-tessellation |
| New stroke | O(points) tessellation once; O(cells) index insert |
| Page open | O(elements) decode + index build, tessellation in parallel workers |

Zoomed-out views use LOD: strokes smaller than ~1 device pixel are skipped or drawn as
simplified meshes; at extreme zoom-out a cached page raster can replace individual items
(profile first).

## 12. Testing hooks

Everything above is driven through `CanvasController` with synthetic events, a fake
`TextLayout`, and a recording `render::Renderer` that captures `RenderFrame`s — see
TESTING.md.
