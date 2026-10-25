# Mirrored room geometry: earlier controlled visual evidence

These screenshots are from the earlier local 0.31.1-based repair on an Apple M1 Max. They document the reported missing floor/wall surfaces and their correction; they are not a claim that the newly rebased upstream PR received a full-game playthrough.

Frigate Biotech Research Area 2 (`C5DE3C06`), frozen camera `(110, 52, -48)`, yaw `0`, pitch `-25`. Both captures submitted 315 draws. The corrected view restores reflected floor panels (`F0E9FCDA`) and wall panels. The importer and original model data were unchanged between these captures.

- `before.png`: the mirrored surfaces are culled away.
- `after.png`: corrected winding/culling restores them.

The upstream contribution has separate synthetic tests for cull-state restoration, merged primitive winding, vertex attributes, index rebasing and primitive splitting. No game model, texture, ISO or NSP files are stored on this evidence branch.
