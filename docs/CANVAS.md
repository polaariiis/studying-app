# Canvas Architecture

> Status: **Phase 4 implemented** (camera, input, scene, pen, select/rectangle/move,
> stroke eraser, pan/zoom, backgrounds); see [§13](#13-phase-4-implementation-notes) for
> what exists and how it differs from the sketches. Phase 6 adds the remaining tools
> (highlighter, partial eraser, lasso, transform handles, shapes, text, connectors).

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
* **Partial eraser** *(implemented in Phase 6, step 3; the default mode)*: cuts the ink
  the eraser covers out of strokes, as vectors. Each pointer step is a capsule (the step's
  segment, eraser radius); a stroke segment is cut exactly where its centre line comes
  within capsule radius + ink radius (`canvas::StrokeEraser`, element-local coordinates),
  so the remaining ink ends at the eraser's edge; surviving points stay bit-exact, cut
  ends get interpolated pressure. Pieces shorter than max(stroke width, 1.5 view px) are
  dropped (no stray dots); a dot is erased whole. The patch
  (`commands::splitStrokes`) keeps the first piece under the stroke's id (an update:
  attached connectors stay attached, fewer id changes than delete + create) and inserts
  further pieces right after it in the draw order; fully erased strokes are removed.
  Style, transform, layer and lock state are kept.
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
* During a drag the canvas draws a preview of the new geometry; on release it commits one
  command (`moveElements`, `resizeElement` or `setConnectorEnds`, each including the
  attached connector updates) as one patch. *(Phase 6, step 8: see §13 "Selection
  handles"; rotation is not implemented.)*
* Cut, copy and paste *(Phase 6, step 9)*: see §13 "Clipboard".
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
* **Implemented (Phase 6, step 5)**: plain UTF-8 text in one style (the application font at
  `kTextSize` = 16 px per world unit, black, `kTextPadding` 4; content JSON v1 unchanged).
  `canvas::TextLayout` (height for a width, rasterise) is implemented by
  `platform::QtTextLayout` on `QTextDocument` and handed to the window by the composition
  root. Text tool (key T): a click edits the box under it or starts a new box 240 wide, a
  drag sets the width. The edit lives in `CanvasController::textEdit()` and in a `QTextEdit`
  overlay (plain text only, same font and wrap width, scaled to the zoom, following pan and
  zoom); the box is hidden on the canvas meanwhile. Escape, Ctrl+Enter or focus leaving the
  editor finish it (a popup — the editor's context menu, a window menu — does not; the
  shell finishes the edit before it changes the page or closes the workspace, so text typed
  before Ctrl+PgUp/PgDn, Ctrl+N or closing is written, not dropped): one command (`createElement`, `commands::editText`, or `deleteElement`
  when cleared; blank new boxes are not created; unchanged text writes nothing); the height
  comes from the layout. The editor takes letters, Delete and Ctrl+Z (its own undo) through
  `ShortcutOverride`, so no tool shortcut or document undo fires while typing. Display: a
  texture per box (`render::createTexture`, drawn on the unit quad), rasterised when its
  content changes (at once) or when the zoom crosses a √2 step (at most 1 Mpx of
  refinements per frame; the old raster is stretched meanwhile and another frame is
  requested), never above 4096 px per side; textures are released with their element or
  page. Text boxes are runs of their own in batches, so painter's order holds. Rotation is
  not shown while editing; resizing is step 8.
* **Font size (1.2, 1.2-TXT-02)**: every text box has its own font size
  (`document::TextBox::fontSize`, a whole number in [6, 144], default `kTextSize` = 16),
  the one source for the layout (`TextLayout::heightFor`/`rasterize` take it), the editor
  overlay's font, the texture cache (a texture remembers the font size and box size it was
  laid out for) and exports. Tools ▸ Text Size (also the style button while the text tool
  is chosen) offers 12–72: the size for new boxes (`ToolSettings::textSize`, remembered
  per user); while a box is edited, that box's size (the editor follows at once; written
  with the text by `commands::editText`); otherwise the selected text boxes' size as one
  command (`commands::setTextFontSize`, "Change text size": their height follows the
  layout, width and text stay). Only the changed boxes are rasterised again.

## 10. Documents and images

* **Implemented (Phase 6, step 6)**: Edit ▸ Insert Image… (Ctrl+Shift+I) checks the file
  with QImageReader, imports it through `WorkspaceSession::importAsset` (content-addressed:
  the same content is stored once and reused) and inserts one selected `Image` element
  (`CanvasController::insertImage`: one world unit per pixel, scaled down to 60 % of the
  view, centred). Pixels come from `canvas::ImageSource`, implemented by
  `ui::SessionImageSource`: the asset path is looked up on the GUI thread, the file is
  decoded on a small thread pool (QImageReader, EXIF orientation, downscaled while
  decoding); a load that is not ready returns nullopt and the source asks the canvas for a
  frame when it is. Decoded images the canvas never collects (the page or the zoom changed
  while decoding) are kept within 256 MB, oldest dropped first (decoded again if asked
  for). The canvas keeps **one texture per asset** (shared by every element showing it)
  at the resolution the view needs, rounded up to a power of two and at most 4096 px
  (2 Mpx of refinements per frame; the coarser texture is stretched meanwhile); zooming
  out keeps the finer texture; least recently drawn textures are released beyond
  256 MB and all of them on a page switch. Missing, corrupted or unsupported assets are
  drawn as the neutral frame and not retried. Images are runs of their own in batches.
  (ARCHITECTURE planned the decoder in `platform`; `ui` may not depend on `platform`, so
  it lives next to `SessionDocumentPort`.)

* **Implemented (Phase 8, D44): PDF pages.** File ▸ Import PDF… reads the file with QtPdf
  (`ui::inspectPdf`: at most 5 000 pages of at most 14 400 pt; password-protected, damaged
  or non-PDF files are refused with a message), imports it as an asset (the original is
  never written) and adds one section with one bounded page per PDF page, sized in world
  units (96/72 per point), as one undo step (`createDocumentSection`, via
  `WorkspaceStructure::importDocument`). The page's `document` names the asset and page;
  annotations are the page's own elements on its layers. The canvas asks a
  `DocumentRasterizer` for tiles: a whole-page preview (the coarsest level at which the page
  fits one 512 px tile) always drawn first, then the 512 px tiles in view at
  `ceil(log2(zoom × DPR))` (levels −8…3, at most 8 px per unit). `SessionDocumentRasterizer`
  renders them on one worker thread (PDFium is serialized anyway) in request order, keeps a
  few documents open, drops queued tiles the canvas no longer wants (`keepOnly`, once per
  frame) and keeps uncollected results within 32 MB / 256 tiles. The controller uploads at
  most 8 tiles per frame (then asks for another frame, until done), keeps tile textures in
  a 192 MB LRU and releases them on a page switch or graphics reset. Unreadable pages show
  the paper and are not retried. Measured (release, integrated GPU laptop): importing a
  200-page PDF takes 106 ms (78 ms of it reading page sizes), a page preview renders in
  ≈ 16 ms and a 512 px tile in ≈ 12 ms off the GUI thread. The dark theme's display
  transform applies to PDF pages like images (D33).
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

*Implemented (Phase 9, D47):* meshes that are new or changed in a frame are built together
before the frame is assembled (`RenderCache::prebuild`: every visible element when
unbatched, every element of the page when batched), split over up to 8 threads with at
least 128 elements each; the merged meshes of rebuilt batch runs are built the same way,
each into its own slot, and uploaded on the GUI thread. Results are identical to building
them one by one (tessellation is pure), and new content is still never deferred to a later
frame. Batch signatures hash whole 64-bit words with a final avalanche; run boundaries keep
the byte-wise hash they were measured with (D40). Selection outlines are built once per
zoom, selection and scene change relative to an anchor and only translated while panning
(`Selection::revision()`). Measured (docs/PERFORMANCE.md): the first frame of a whole
10 000-stroke page 237 → 88 ms, a frame while panning with 10 000 elements selected
8.5–10.5 → 0.2–0.5 ms, a stroke's commit frame 4.5 → 3.4 ms.

## 12. Testing hooks

Everything above is driven through `CanvasController` with synthetic events, a fake
`TextLayout`, and a recording `render::Renderer` that captures `RenderFrame`s — see
TESTING.md.

## 13. Phase 4 implementation notes

What exists (`src/canvas`), and where it differs from the sketches above.

* **Camera** (§2) as sketched, plus `viewToDevice`/`deviceToView`, `worldToViewTransform`
  and `reset()` (zoom 1, world origin at the top-left). Positions are `DVec2` in every space.
  Bounded pages are fitted on open (32 px margin) and clamped so ≥ 48 px of the page stays
  visible. Since Phase 5 the shell remembers each page's camera for the session and
  restores it with `CanvasController::setView`; it is not persisted (ARCHITECTURE.md D37).
* **Input** (§4): `PointerEvent` (phase, device incl. the pen's eraser end, button,
  view position, pressure, modifiers, timestamp, pointer id), `WheelEvent` (angle and
  pixel deltas), `ZoomGestureEvent`, `KeyEvent` (Space, Escape, Delete). Tilt and
  coalesced samples are not used yet. Wheel = zoom around the cursor (×1.0015 per 1/8°);
  touchpad pixel scrolling pans; Ctrl+wheel always zooms; pinch zooms; middle button or
  Space+drag pans with any tool.
* **Document access**: tools get a `DocumentPort` (decision D30) instead of an `Editor`;
  the owner reports every applied patch to `CanvasController::onDocumentChanged`, which asks
  for a frame only if the patch changes what the canvas shows (elements, layers, the open
  page's format or background): planner edits — tasks, tags, page tags — cost no frame
  (Phase 7). One gesture commits at most one command. Read-only ports refuse edits in the tools.
* **Pen** (§5.1): dedupe < 0.5 view px → One-Euro (min cutoff 2 Hz, β 0.02 per view px/s,
  derivative cutoff 1 Hz; missing timestamps assume 120 Hz) → on pen-up the last raw
  sample is appended (no end lag) → RDP with ε = 0.25 view px that also keeps points whose
  pressure deviates > 0.05 from the chord. Thresholds are view pixels converted with the
  zoom at stroke start; stored geometry is world units (points relative to the first
  point, which becomes the element position). Width = `baseWidth` × (0.3 + 0.7 ×
  pressure); mice report pressure 1. Strokes start only inside a bounded page.
* **Tool settings** *(Phase 6, step 1)*: `canvas::ToolSettings` (the pen's and the
  highlighter's brush, colour and width) is tool state owned by the controller and sanitized
  there (width 0.25–64 world units, never fully transparent). It decides new strokes only: each
  stroke stores its own brush, colour and width in the document, so changing the settings
  never changes content, is not an edit, and rendering reads only the document. A stroke
  keeps the style it started with. The shell offers a short list of muted inks and three
  widths (`inkPalette()`, `penWidthPresets()` in DesignTokens) in one toolbar button and
  Tools ▸ Pen Style, and remembers the choice per user (QSettings), across workspaces.
* **Highlighter** *(Phase 6, step 2)*: `ToolKind::Highlighter` is the pen tool with its
  own settings (`ToolSettings::highlighter`, default `kDefaultHighlighter`: yellow at 45 %
  opacity, 14 units) and cursor (`CursorShape::Highlighter`, a platform cursor image like
  the eraser ring). Its strokes are ordinary strokes with `document::Brush::Highlighter`
  and a translucent colour; brush, colour (with alpha) and width are stored per stroke, so
  they reopen as drawn without any tool state, and no schema change was needed (the brush
  column already had the value). The highlighter ignores pressure — an even band
  (`strokeRadius`), while the recorded pressure is kept in the points; the pen's pressure
  response is unchanged. Rendering is the normal stroke path: translucency comes from the
  stored alpha, and the renderer's single-coverage rule (RENDERING.md §6.5) keeps a stroke
  even where it overlaps itself; separate strokes build up where they cross, and ink under
  a highlighter stays visible (normal alpha blending, drawn in document order — a
  highlighter drawn *under* later ink is covered by it). The shell adds a Highlighter tool
  (key M) and Tools ▸ Highlighter Style (four light inks, Fine/Medium/Thick = 8/14/22);
  the toolbar's one style button shows the style of the ink tool chosen last. Choosing a
  tool or a style is never an edit and rebuilds no geometry; a style change repaints
  nothing (a tool change requests one frame, as before, e.g. for a cancelled gesture).
* **Shapes** *(Phase 6, step 4)*: `ToolKind::Shape` drags out a line, arrow, rectangle
  or ellipse (`canvas::shapeFromDrag`, `ToolSettings::shape`: kind, outline colour and
  width, optional fill = the outline colour at 20 %). Boxes are stored with their top-left
  corner as position; lines and arrows with the press point as position, their length
  along local +x and their direction as the element rotation (`Shape::size` stays
  non-negative). Shift makes boxes square and snaps lines to 45°; drags shorter than
  3 view px create nothing. `ShapeKind::Arrow` (stored value 6, no migration) draws a
  shaft and a filled head (3.5 × width, ≥ 8, ≤ 60 % of the length) as one mesh. The
  preview is built from the dragged geometry each step (a few triangles, two scratch
  meshes) and never enters the document; release commits one `createElement`. The scene
  indexes `visualBounds()` (document bounds plus half the outline and the arrowhead), so
  culling, selection frames and spatial queries include the ink outside the box. Shell:
  Shape tool (key S) and Tools ▸ Shape Style (kinds, the pen inks, widths, Fill),
  remembered per user; the style button follows pen, highlighter or shape, whichever was
  chosen last.
* **Connectors** *(Phase 6, step 7)*: `ToolKind::Connector` (key C) drags a straight
  connector with an arrowhead at its end (the schema's default end cap), in the shape
  style's colour and width. An end pressed or released on an element (not a connector,
  not locked; boxes anywhere inside, ink near it) attaches to it, at the point where the
  line towards the other end leaves the element's visual bounds (`attachPoint`); elsewhere
  the end is free. Pressing within 6 view px of an end of an existing connector drags that
  end (`commands::setConnectorEnds`: detach, re-attach; the fixed attached end is re-aimed).
  Routing stays simple: when an element moves, `moveElements` translates the attached
  ends by the same delta, finding the connectors through the workspace's attachment index
  (`Workspace::connectorsAttachedTo`, element → connectors) instead of scanning the page,
  so a move costs O(moved + attached). During a move preview the attached connectors are
  drawn with their ends following the offset; they are found once when the move starts.
  Deleting an element detaches its connectors (the connector stays).
* **Selection handles** *(Phase 6, step 8)*: a single selected, unlocked element on an
  editable workspace shows handles (`canvas::handlesFor`, 7 view px squares, grabbed within
  6 view px; handles win over hit testing of what lies below): rectangles, ellipses and
  images 8 (corners and edges), text boxes left and right (the width; the height follows
  the text layout), lines, arrows and connectors their two ends. Rotated or scaled
  elements and strokes have none. `dragHandle` is pure geometry: the opposite corner or
  edge stays (dragging past it flips the box), images keep their aspect ratio on corners
  (Shift frees it; for shapes Shift keeps it), line ends snap to 45° with Shift, sizes never
  drop below 4 view px. The drag draws the element at its new geometry (shapes
  tessellated per frame, images stretch their texture; since 1.2 a text box is laid out
  and rasterised again for its new width, as its raster is cached with the size it was
  laid out for) and writes nothing;
  release commits `commands::resizeElement` (connector ends attached to the element keep
  their relative place on its bounds) or, for a connector end, `setConnectorEnds` with the
  end re-attached to whatever it is dropped on. Limitation: attached connectors are
  redrawn at their new ends on release, not during the drag.
* **Clipboard** *(Phase 6, step 9; D41)*: Edit ▸ Cut/Copy/Paste (Ctrl+X/C/V while the
  canvas has focus; the text editor keeps those keys for its text). Copy stores the
  selected elements' values in painter order in the controller's clipboard, which belongs
  to the open workspace (not the system clipboard) and survives page switches. Paste is one
  `commands::pasteElements` command onto the page's target layer: new ids, the copied
  order on top of the layer, connector ends between copied elements re-attached to the
  copies through a temporary old → new id map, ends on anything else detached in place,
  images referencing the same asset (no second stored file). The copies are selected and
  the Select tool is chosen. Placement: on the source page each paste is 16 view px further
  down and right (after a cut the first paste is in place), on another page the copies keep
  their coordinates; they are centred in the view instead if that would put them out of
  sight (on another page: not entirely in view). Cut
  is copy + delete (one command, labelled "Cut"). Read-only workspaces copy but do not cut or paste.
  Measured (release, `BM_CopyPaste`, 10 000-stroke page): 1 014 strokes copy in 1.4 ms and
  paste in 2.5 ms (command, apply, scene), the frame after tessellates them in 18.6 ms; the
  whole page copies in 7.8 ms, pastes in 26 ms, and its next frame takes 177 ms (the
  first-frame cost of 10 000 new strokes, Phase 9).
* **Single-key tool shortcuts** (P, M, S, T, C, V, E, H, Z) are window shortcuts, except while
  the navigation tree has focus: there plain typing is the tree's keyboard search
  (`NavigationPanel` takes those keys through `ShortcutOverride`). Text fields already
  take them the same way; chords (Ctrl+Z, …) work everywhere.
* **Eraser** (§5.2): 8 view px radius, one patch on release. *(Phase 6, step 3)*
  `ToolSettings::eraser` chooses `EraserMode::Partial` (default) or `WholeStroke` (the
  Phase 4 stroke eraser); Tools ▸ Eraser in the shell, remembered per user. Candidates
  come from the scene's spatial grid for each step's bounds; a candidate is only copied
  into the gesture's working state (`Preview::partial`, keyed by id) once it is actually
  cut. Cut strokes are hidden and drawn from their pieces in their place (one preview
  mesh per stroke, re-tessellated only when that stroke changed in the step). Its reach is shown by
  `CursorShape::EraserRing`, which the UI turns into a platform cursor image: a ring drawn
  by the renderer trailed the pointer by the one to two frames every composited window
  presents late (≈ 15–45 px at 2 000 px/s, measured), stayed behind when the pointer left
  the canvas, and cost a frame per hover move. Hover moves therefore request no repaint.
* **Selection** (§7): click (4 view px tolerance, topmost, Shift toggles), rectangle
  (intersection with the ink by default, Alt = bounds fully contained, Shift adds), move
  by dragging a selected element (3 px threshold; preview offset, one `moveElements`
  command), Delete/Backspace (`deleteElements`). Selection is pruned when patches remove
  elements. Lasso, handles and snapping are Phase 6.
* **Scene** (§3): `CanvasScene` mirrors the displayed page of the (whole, in-memory)
  `Workspace` — there is no `PageDocument` yet (D25). Spatial grid as §3.1 (512-unit
  cells, > 16 cells → "large" list; queries sorted and exact on bounds). Draw order is the
  Workspace's ordered indexes (layers, then elements), so no extra sort. Content versions
  change only when a payload changes (moves keep the mesh).
* **Render cache** (§3.3): CPU meshes per element keyed by content version and a
  power-of-two zoom bucket; rebuilt only for new content or when drawn at a finer bucket.
  GPU handles of removed or rebuilt meshes are destroyed at the next frame. Refinement for
  a finer bucket is spread over frames (at most 256 meshes per frame; the coarser mesh,
  which has the same world geometry, is drawn meanwhile and the controller requests
  frames until it is done); new or changed content is always built at once.
* **Batching** (D31): above 1 024 visible elements, `RenderBatches` draws runs of up to
  256 consecutive same-layer elements as one mesh; runs touched by a move/erase preview
  are drawn per element, except runs that move as a whole (every member selected), which
  stay one draw moved by the preview offset.
* **Hit testing** (§6): strokes by distance to the transformed polyline (per-point
  radius + tolerance); shapes, text boxes and images by their local boxes; connectors by
  segment distance. Locked layers are not hit-testable; hidden layers are skipped.
* **Profiling**: `CanvasController::profiler()` records "build frame", "scene query",
  "prepare content", "live stroke" and "scene update" (`STUDYAPP_PROFILE_SCOPE`).
