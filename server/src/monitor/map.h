/* map.h — the Monitor's world map drawn from vectors (see map.c and docs/MONITOR.md).
 *
 * Country outlines (assets/monitor/land.bin, Natural Earth 1:50m) are filled with
 * antialiasing at any zoom, with borders, arcs from "this machine" and one dot per place. All on
 * its own thread: the screen thread requests a frame (map_request) and later collects it
 * (map_take) when it's ready, without waiting. */
#ifndef LORD_MAP_H
#define LORD_MAP_H

#include <stdint.h>

#define MAP_MAX_W 2048
#define MAP_MAX_H 2048
#define MAP_MAX_DOTS 256
#define MAP_MAX_ARCS 512

typedef struct { float lon, lat, r; int sel, idle; } MapDot;   /* idle: no traffic now (dimmed arc) */
/* (0.8.5) one app's arc to a place: its own color and lift (a fraction of the length), so the apps that
 * reach the same country get arcs apart */
typedef struct { float lon, lat, lift; uint32_t color; int sel, idle; } MapArc;

typedef struct {
    int w, h;
    double lon0, lat0, ppd;             /* top-left corner and pixels per degree */
    uint32_t land, border, accent, home_color, sel_color;   /* 0xRRGGBBAA */
    int has_home;
    double hlon, hlat;
    int ndots;
    MapDot dots[MAP_MAX_DOTS];
    int narcs;                          /* 0: one arc per dot, in the accent color */
    MapArc arcs[MAP_MAX_ARCS];
} MapReq;

void map_init(const char *assets_dir);
/* Requests a frame (replaces the previous request if it hasn't started yet). Returns its number. */
uint64_t map_request(const MapReq *r);
/* The last finished frame, if newer than *seq: a copy (malloc) in *rgba; 1 if there is one. */
int map_take(uint64_t *seq, uint8_t **rgba, MapReq *view, double *ms);

#endif
