#ifndef PROJECTION_HPP
#define PROJECTION_HPP

void lonlat2tile(double lon, double lat, int zoom, long long *x, long long *y);
void epsg3857totile(double ix, double iy, int zoom, long long *x, long long *y);
void tile2lonlat(long long x, long long y, int zoom, double *lon, double *lat);
void tiletoepsg3857(long long x, long long y, int zoom, double *ox, double *oy);
void epsg4490totile(double lon, double lat, int zoom, long long *x, long long *y);
void tiletoepsg4490(long long x, long long y, int zoom, double *lon, double *lat);
void set_projection_or_exit(const char *optarg);
void set_output_projection_or_exit(const char *optarg);
struct projection *find_projection(const char *name);

struct projection {
	const char *name;
	void (*project)(double ix, double iy, int zoom, long long *ox, long long *oy);
	void (*unproject)(long long ix, long long iy, int zoom, double *ox, double *oy);
	const char *alias;
	bool geographic;   // true: project() takes longitude/latitude in degrees
	bool linear_grid;  // true: equirectangular 2^z x 2^(z-1) grid (EPSG:4490 style)
};

extern struct projection *projection;  // input projection (-s)
extern struct projection projections[];

bool output_grid_is_linear(void);
struct projection *get_output_projection(void);

// Project input coordinates (per -s: lon/lat for 4326/4490, Web Mercator
// meters for 3857) to output-grid tile coordinates.
void project_input(double ix, double iy, int zoom, long long *ox, long long *oy);
// Unproject output-grid tile coordinates to longitude/latitude.
void unproject_output_ll(long long ix, long long iy, int zoom, double *ox, double *oy);
// If the output grid is equirectangular, fill TileMatrixSet metadata and
// return true; return false for the standard Web Mercator grid.
bool output_tile_matrix_meta(const char **crs, double *origin_x, double *origin_y, double *z0_dim);

extern unsigned long long (*encode_index)(unsigned int wx, unsigned int wy);
extern void (*decode_index)(unsigned long long index, unsigned *wx, unsigned *wy);

unsigned long long encode_quadkey(unsigned int wx, unsigned int wy);
void decode_quadkey(unsigned long long index, unsigned *wx, unsigned *wy);

unsigned long long encode_hilbert(unsigned int wx, unsigned int wy);
void decode_hilbert(unsigned long long index, unsigned *wx, unsigned *wy);

unsigned long long encode_vertex(unsigned int wx, unsigned int wy);

#endif
