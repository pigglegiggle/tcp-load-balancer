# Architecture diagrams

Three separate text-first diagrams accompany the README. They use white rectangular nodes, black text, and straight or orthogonal connectors, without icons, gradients or decorative illustrations.

| Topic | README image | Editable source |
|---|---|---|
| Process boundaries, data plane and reactor control | [System architecture](images/system-architecture.svg) | [Open in draw.io](images/system-architecture.drawio) |
| Independent forwarding buffers, backpressure and half-close | [Data path](images/data-path.svg) | [Open in draw.io](images/data-path.drawio) |
| Indexed heap and probe-based eligibility | [Backend selection](images/backend-selection.svg) | [Open in draw.io](images/backend-selection.drawio) |

Open a `.drawio` file in diagrams.net or the draw.io desktop application to edit individual boxes, text and connectors. Export as SVG and replace the matching README asset after changes.

The architecture overview separates the established TCP byte path (solid arrows) from setup, health and reactor control (dashed arrows). Selector and listener boxes are setup/control responsibilities; application bytes do not flow through them after connection setup. The tunnel routes to one selected member of the backend pool. Health probes are separate TCP connections.

The heap values are an illustrative snapshot, not benchmark data. Connecting and established traffic leases both count as active; probes do not. The health diagram shows consecutive probe thresholds; a passive connection failure excludes its backend immediately. Existing streams are never moved by a health transition.

These final assets were constructed as native SVG and editable draw.io XML in response to the requested simple diagram style. AI-generated raster drafts were discarded and are not included. The detailed behavior and invariants are documented in [ARCHITECTURE.md](ARCHITECTURE.md).
