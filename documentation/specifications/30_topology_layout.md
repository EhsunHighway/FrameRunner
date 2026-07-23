# Module 30 - Automatic Topology Layout

**Files:** `src/display/topology_layout.c`, `src/display/topology_layout.h`
**Status:** Ready for implementation; files do not exist yet
**Depends on:** `topology`, `device`, `interface`, `link`

## Concepts First

The runtime topology is a graph: devices are vertices and links are edges. A
terminal renderer needs coordinates, but those coordinates are presentation
state rather than network state. A deterministic layered layout derives them
from connectivity whenever a topology or viewport is loaded.

Connected components are independent graph regions. Breadth-first depth gives
each device a stable column within its component; stable tie-breaking keeps
the same topology from moving between runs.

## Purpose

The topology-layout module calculates temporary terminal positions for every
device in a topology. Users describe network connectivity only; they never
provide visual coordinates.

Layout is deterministic, read-only with respect to the network, and safe to
rebuild when the terminal changes size. It does not transmit packets, schedule
events, or store coordinates in `Device`, `Interface`, `Link`, or topology
configuration records.

## Data Model

```c
typedef struct TopologyLayoutNode {
    const Device *device;
    int           x;
    int           y;
    int           component;
    int           depth;
} TopologyLayoutNode;

typedef struct TopologyLayout {
    TopologyLayoutNode *nodes;
    size_t              node_count;
    int                 canvas_width;
    int                 canvas_height;
    int                 content_width;
    int                 content_height;
} TopologyLayout;
```

The layout owns its node array and borrows device pointers. The topology and
all devices must outlive the layout.

## Ownership And Lifetime

`TopologyLayout` owns only its node array and scalar dimensions. Each node
borrows a `Device *` from the topology used by the last successful rebuild.
The CLI must free the layout before freeing or replacing that simulator.

## Public API

```c
TopologyLayout *topology_layout_create(const Topology *topology,
                                       int             canvas_width,
                                       int             canvas_height);

int topology_layout_rebuild(TopologyLayout *layout,
                            const Topology  *topology,
                            int              canvas_width,
                            int              canvas_height);

const TopologyLayoutNode *topology_layout_find(
    const TopologyLayout *layout,
    const Device         *device);

void topology_layout_free(TopologyLayout *layout);
```

## Deterministic Layout Algorithm

The first milestone uses a layered breadth-first layout. It prioritizes
predictability and testability over an optimal graph drawing.

Implementation order for `topology_layout_rebuild`:

1. Reject null input, nonpositive canvas dimensions, negative topology counts,
   or live topology ranges containing an unusable device pointer.
2. Allocate temporary adjacency information from
   `topology->devices[0 .. dev_count - 1]` and
   `topology->links[0 .. link_count - 1]`. Ignore a link with a null endpoint
   for layout adjacency but preserve the topology itself unchanged.
3. Find connected components in increasing topology device-index order. The
   first still-unassigned device starts the next component.
4. For each component, choose one root:
   - select the device with the highest number of links to devices in the same
     component
   - when degrees tie, select the lexicographically lower device name
5. Run breadth-first traversal from the root. Store graph distance as `depth`.
   For equal-depth nodes, order by parent order and then device name.
6. Assign one horizontal column per depth. Assign vertical positions by evenly
   spacing the nodes in each column within the component region.
7. Pack connected components from top to bottom in component-discovery order,
   leaving a defined minimum gap between them.
8. Record content width and height even when they exceed the visible canvas.
   Do not discard nodes merely because the topology is larger than the
   terminal.
9. Replace the old node array only after the complete new layout succeeds.
10. Release temporary adjacency and traversal storage and return `0`.

`topology_layout_create` allocates an empty layout, calls rebuild, and frees the
layout on rebuild failure.

## Function Behavior

### `topology_layout_create`

1. Reject a null topology or nonpositive canvas dimension with `NULL`.
2. Allocate and zero one layout.
3. Call `topology_layout_rebuild` with the supplied topology and dimensions.
4. If rebuild fails, free the layout and return `NULL`.
5. Return the completed layout.

### `topology_layout_rebuild`

Follow the ten ordered algorithm steps above. The existing node array and all
existing fields remain unchanged on failure. On success, free the old node
array only after the temporary replacement is complete, publish all new
fields together, and return `0`.

### `topology_layout_find`

1. If `layout == NULL || device == NULL`, return `NULL`.
2. Scan `nodes[0 .. node_count - 1]` in increasing index order.
3. Return the borrowed address of the first node whose `device` pointer equals
   the requested pointer.
4. Return `NULL` if no node matches.

### `topology_layout_free`

1. If `layout == NULL`, return without action.
2. Free the owned node array.
3. Free the layout. Do not free any borrowed device.

## Large Topologies

The layout reports content dimensions independently from canvas dimensions.
Animation owns viewport offset, panning, clipping, focus, and packet
aggregation. Layout does not hide devices or alter coordinates based on the
currently focused packet.

When minimum spacing cannot fit in the canvas, coordinates remain in the larger
content space. The renderer shows the visible viewport.

## Device Type

When `DeviceType` is available, renderers may choose different node labels or
shapes. The first layered placement algorithm does not require a device type
and therefore also works with generic devices.

## Verification Expectations

Tests cover empty, one-node, line, star, cycle, disconnected, null-endpoint,
small-canvas, and oversized topologies. Rebuilding the same graph and canvas
must produce the same component, depth, x, and y values.

## ACSL Contract Targets

Contracts must express positive dimensions on success, one node per topology
device, borrowed device pointers, lookup returning only an element inside the
logical node range, and failure preserving the previous layout.

## Common Mistakes

- Adding `x` and `y` fields to `Device` or parsing them from the scenario.
- Choosing a root or traversal neighbor using pointer address order.
- Destroying the old layout before a replacement has successfully completed.
- Dropping nodes that do not fit the current terminal instead of reporting
  larger content dimensions.
