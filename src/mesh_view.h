#pragma once
// "MeshCore": the owner's MeshCore repeater at the centre, its neighbour repeaters around it
// by bearing and distance over the weather map's roads, and light running along each link as
// often as it is heard. Turning the knob steps from neighbour to neighbour (clockwise from
// north) and shows that link's figures; after a few quiet seconds the overview returns.
//
// The node is chosen at http://theorb.local/meshcore (main.cpp keeps it in NVS) and shown or
// hidden in Settings > MeshCore. The data is the MeshCore analyzer's (config.h, MESH_API_BASE).
#include <lvgl.h>

namespace meshview {
void      init();
lv_obj_t *screen();
void      onEnter();
void      onExit();
void      onTurn(int delta);
void      setShown(bool shown);
void      setNodeId(const char *id);   // 64 hex characters, or "" for none; refetches at once

// The network side, called from the feed task (core 0), which owns the sockets.
bool      fetchWanted();               // shown, open, a node is set, and the data is stale
void      fetch();                     // blocking: two HTTPS GETs, parsed into the page's data
}
