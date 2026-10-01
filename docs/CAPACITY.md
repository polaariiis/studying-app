# Capacity

How much content StudyBoard handles well, based only on measurements. StudyBoard has no
hard limits on the number of strokes, pages, images or tasks; what changes with size is how
responsive it stays. Every statement below comes from [STRESS_TESTING.md](STRESS_TESTING.md)
or [BENCHMARKS.md](BENCHMARKS.md), measured on **one machine**: the reference laptop in
[HARDWARE_MATRIX.md](HARDWARE_MATRIX.md) (Ryzen 5 8645HS, 8 GB RAM, integrated Radeon 760M,
144 Hz display, Windows 10). Slower or faster machines shift every boundary; these are not
guarantees for other hardware.

## Categories

| Category | Meaning (on the reference laptop) |
|---|---|
| **Comfortable** | Panning stays within the 144 Hz frame budget (6.9 ms, CPU side, 95th percentile) at every zoom; opening the workspace and the first whole-page frame each take ≤ 250 ms |
| **Supported** | Panning stays within the 60 Hz budget (16.7 ms, p95) at every zoom; opening and the first whole-page frame each take ≤ 1 s |
| **Stress** | Everything works, but some views drop below 60 frames per second or single operations take more than a second |
| **Degradation / limit** | A failure, or degradation that makes the workload unusable. **Not reached** in any test: the largest measured size (100 000 strokes on one page) still works |

Frame times are the CPU side of rendering (the GPU part was measured in the application
only for 10 000 strokes, where the whole page pans at ≥ 60 fps).

## Strokes on one page

Strokes are handwriting-like (30–90 points each) spread over one page of about
6000 × 3600 units.

| Strokes | Category | Pan p95, zoom 1 / whole page | Open workspace | First whole-page frame | Move all + save | Working set* |
|---:|---|---|---:|---:|---:|---:|
| 1 000 | Comfortable | 0.04 / 0.29 ms | 14 ms | 24 ms | 11 ms | 20 MB |
| 5 000 | Comfortable | 0.25 / 0.93 ms | 51 ms | 58 ms | — | 70 MB |
| 10 000 | Comfortable | 0.57 / 1.99 ms | 105 ms | 99 ms | 123 ms | 128 MB |
| 25 000 | Supported | 1.19 / 5.71 ms (8.8 ms with all selected) | 250 ms | 226 ms | — | 301 MB |
| 50 000 | Stress | 2.44 / 20.0 ms | 552 ms | 491 ms | 807 ms | 588 MB |
| 100 000 | Stress | 7.73 / 31.1 ms | 1.22 s | 1.06 s | 1.48 s | 1 148 MB |

\* Working set of the benchmark process after the first whole-page frame. It counts the
meshes twice (the benchmark's recording renderer keeps a CPU copy that the real OpenGL
renderer keeps in video memory instead), so the application's RAM use is lower by roughly
the mesh size: 50 MB at 10 000 strokes, 500 MB at 100 000. The application's own start-up
working set is about 260 MB, mostly the OpenGL driver ([PERFORMANCE.md](PERFORMANCE.md)).
"—": not measured at that size.

* **Bottleneck:** the number of strokes *in view*. At normal zoom only a small part of the
  page is drawn, so even 100 000 strokes pan within 60 Hz; with the whole page in view,
  culling and batch bookkeeping grow with every visible stroke.
* **Memory:** about 5 MB of meshes per 1 000 strokes in view at the whole-page level of
  detail, plus the document itself.
* **Interactive:** yes at every measured size at normal zoom. Whole-page views above about
  25 000 strokes are below 60 fps.
* **Beyond 100 000:** not measured. Costs grew roughly linearly from 10 000 to 100 000
  strokes; extending that trend (an extrapolation, not a measurement) predicts about 2 s to
  open and 60 ms per whole-page frame at 200 000.

Real notes are usually split over many pages; a page's cost depends on its own content,
and only the page shown is kept in the render cache.

## Text boxes (search)

| Text boxes in the workspace | Search (50 results) | Category |
|---:|---:|---|
| 1 000 | 0.30 ms | Comfortable |
| 10 000 | 2.2 ms | Comfortable |
| 100 000 | 23 ms | Comfortable (typing is debounced by 150 ms) |

Rasterising one text box takes 0.13 ms at 1× and 1.7 ms at 4.2× zoom; refinement is limited
to about 1 megapixel per frame, so many text boxes appear progressively sharper rather than
slowing a frame down. Pages with very many text boxes on the canvas were **not measured**.

## Images

| Image | Import (copy + hash, off the GUI thread) | Time on the GUI thread |
|---|---:|---:|
| 512 × 512 PNG, 0.8 MB | 18 ms | — |
| 4096 × 4096 PNG, 50 MB | 394 ms | 2.2 ms |

Decoded image textures are limited to 256 MB (least recently drawn first out); images are
decoded off the GUI thread. Pages with many large images were **not measured**.

## PDF documents

| Document | Operation | Time |
|---|---|---:|
| 200 pages | Import: reading the page sizes | 55 ms |
| any | Render one tile: preview / level 1 | 6.4 / 3.9 ms, on a worker thread |

Rendered tile textures are limited to 192 MB, and at most four PDF documents are kept open
by the renderer. PDFs larger than 200 pages were **not measured**.

## Planner

Creating a task (including the database write) takes 0.33 ms with 100, 1 000 or 10 000
tasks in the workspace — no measurable growth. Larger task sets were **not measured**.

## Bundles

A notebook with 10 000 strokes: exporting as a bundle 299 ms, importing 465 ms. Since 1.2
(docs/BENCHMARKS.md "Workspace maintenance"): a ~1 GB workspace exports in 5.4 s, opens as a
workspace in 11.8 s and imports in 21.5 s; **a workspace whose bundle would exceed 4 GiB
cannot be exported** (no zip64, D46; a ~5 GB workspace fails after 24 s).

## Workspace size (maintenance)

Measured in 1.2 on ~1 GB and ~5 GB workspaces (docs/PERFORMANCE.md §3.11): Back Up Now and
the daily backup on close cost about 0.2 s per GB of database (≈ 1 s at 5 GB) — comfortable.
Check Workspace (6.5 s per GB of assets) and Save a Copy (9 s per GB) are linear in the
asset bytes and freeze the window meanwhile: tolerable at a few hundred MB, "Not
Responding" from about 1 GB.

## Export

Exporting a page costs time proportional to its strokes (PDF: 35 ms per 100 strokes,
3.1 s for 10 000) and, for PNG, to its area (up to 64 megapixels, 5–12 s for a page 6000
units wide). A typical A4 page with 1 000 strokes: PDF 0.35 s, PNG 0.87 s, SVG 0.53 s.
Export runs with a progress dialog and can be cancelled.

## Summary

On the reference laptop, pages of up to about 10 000 strokes are comfortable in every view,
25 000 are supported, and 50 000–100 000 still work but whole-page views and a few
operations (open, moving everything, export) become noticeably slower. No workload tested
up to 100 000 strokes failed.
