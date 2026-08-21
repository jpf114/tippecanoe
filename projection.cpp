#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <cmath>
#include <atomic>
#include "projection.hpp"
#include "errors.hpp"

unsigned long long (*encode_index)(unsigned int wx, unsigned int wy) = NULL;
void (*decode_index)(unsigned long long index, unsigned *wx, unsigned *wy) = NULL;

struct projection projections[] = {
	{"EPSG:4326", lonlat2tile, tile2lonlat, "urn:ogc:def:crs:OGC:1.3:CRS84", true, false},
	{"EPSG:4490", epsg4490totile, tiletoepsg4490, "urn:ogc:def:crs:EPSG::4490", true, true},
	{"EPSG:3857", epsg3857totile, tiletoepsg3857, "urn:ogc:def:crs:EPSG::3857", false, false},
	{NULL, NULL, NULL, NULL, false, false},
};

struct projection *projection = &projections[0];

// http://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
void lonlat2tile(double lon, double lat, int zoom, long long *x, long long *y) {
	// Place infinite and NaN coordinates off the edge of the Mercator plane

	int lat_class = std::fpclassify(lat);
	int lon_class = std::fpclassify(lon);
	bool bad_lon = false;

	if (lat_class == FP_INFINITE || lat_class == FP_NAN) {
		lat = 89.9;
	}
	if (lon_class == FP_INFINITE || lon_class == FP_NAN) {
		// Keep these far enough from the plane that they don't get
		// moved back into it by 360-degree offsetting

		lon = 720;
		bad_lon = true;
	}

	// Must limit latitude somewhere to prevent overflow.
	// 89.9 degrees latitude is 0.621 worlds beyond the edge of the flat earth,
	// hopefully far enough out that there are few expectations about the shape.
	if (lat < -89.9) {
		lat = -89.9;
	}
	if (lat > 89.9) {
		lat = 89.9;
	}

	if (lon < -360 && !bad_lon) {
		lon = -360;
	}
	if (lon > 360 && !bad_lon) {
		lon = 360;
	}

	double lat_rad = lat * M_PI / 180;
	unsigned long long n = 1LL << zoom;

	long long llx = std::round(n * ((lon + 180) / 360));
	long long lly = std::round(n * (1 - (log(tan(lat_rad) + 1 / cos(lat_rad)) / M_PI)) / 2);

	*x = llx;
	*y = lly;
}

// http://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
void tile2lonlat(long long x, long long y, int zoom, double *lon, double *lat) {
	unsigned long long n = 1LL << zoom;
	*lon = 360.0 * x / n - 180.0;
	*lat = atan(sinh(M_PI * (1 - 2.0 * y / n))) * 180.0 / M_PI;
}

void epsg3857totile(double ix, double iy, int zoom, long long *x, long long *y) {
	// Place infinite and NaN coordinates off the edge of the Mercator plane

	int iy_class = std::fpclassify(iy);
	int ix_class = std::fpclassify(ix);

	if (iy_class == FP_INFINITE || iy_class == FP_NAN) {
		iy = 40000000.0;
	}
	if (ix_class == FP_INFINITE || ix_class == FP_NAN) {
		ix = 40000000.0;
	}

	*x = std::round(ix * (1LL << 31) / 6378137.0 / M_PI + (1LL << 31));
	*y = std::round(((1LL << 32) - 1) - (iy * (1LL << 31) / 6378137.0 / M_PI + (1LL << 31)));

	if (zoom != 0) {
		*x = std::round((double) *x / (1LL << (32 - zoom)));
		*y = std::round((double) *y / (1LL << (32 - zoom)));
	}
}

void tiletoepsg3857(long long ix, long long iy, int zoom, double *ox, double *oy) {
	if (zoom != 0) {
		ix <<= (32 - zoom);
		iy <<= (32 - zoom);
	}

	*ox = (ix - (1LL << 31)) * M_PI * 6378137.0 / (1LL << 31);
	*oy = ((1LL << 32) - 1 - iy - (1LL << 31)) * M_PI * 6378137.0 / (1LL << 31);
}

// EPSG:4490 (CGCS2000) equirectangular tile grid: linear in both longitude and
// latitude. Each zoom level has 2^z columns and 2^(z-1) rows. Truncation (not
// rounding) matches the legacy tiling tool's row/column numbering exactly.
void epsg4490totile(double lon, double lat, int zoom, long long *x, long long *y) {
	int lat_class = std::fpclassify(lat);
	int lon_class = std::fpclassify(lon);

	if (lat_class == FP_INFINITE || lat_class == FP_NAN) {
		lat = 90;
	}
	if (lon_class == FP_INFINITE || lon_class == FP_NAN) {
		lon = 360;
	}

	if (lat < -90) {
		lat = -90;
	}
	if (lat > 90) {
		lat = 90;
	}
	if (lon < -360) {
		lon = -360;
	}
	if (lon > 360) {
		lon = 360;
	}

	unsigned long long n = 1LL << zoom;

	long long llx = n * ((lon + 180) / 360);
	long long lly = n * ((90 - lat) / 360);

	*x = llx;
	*y = lly;
}

void tiletoepsg4490(long long x, long long y, int zoom, double *lon, double *lat) {
	unsigned long long n = 1LL << zoom;
	*lon = 360.0 * x / n - 180.0;
	*lat = 90.0 - 360.0 * y / n;
}

// https://en.wikipedia.org/wiki/Hilbert_curve

void hilbert_rot(unsigned long long n, unsigned *x, unsigned *y, unsigned long long rx, unsigned long long ry) {
	if (ry == 0) {
		if (rx == 1) {
			*x = n - 1 - *x;
			*y = n - 1 - *y;
		}

		unsigned t = *x;
		*x = *y;
		*y = t;
	}
}

unsigned long long hilbert_xy2d(unsigned long long n, unsigned x, unsigned y) {
	unsigned long long d = 0;
	unsigned long long rx, ry;

	for (unsigned long long s = n / 2; s > 0; s /= 2) {
		rx = (x & s) != 0;
		ry = (y & s) != 0;

		d += s * s * ((3 * rx) ^ ry);
		hilbert_rot(s, &x, &y, rx, ry);
	}

	return d;
}

void hilbert_d2xy(unsigned long long n, unsigned long long d, unsigned *x, unsigned *y) {
	unsigned long long rx, ry;
	unsigned long long t = d;

	*x = *y = 0;
	for (unsigned long long s = 1; s < n; s *= 2) {
		rx = 1 & (t / 2);
		ry = 1 & (t ^ rx);
		hilbert_rot(s, x, y, rx, ry);
		*x += s * rx;
		*y += s * ry;
		t /= 4;
	}
}

unsigned long long encode_hilbert(unsigned int wx, unsigned int wy) {
	return hilbert_xy2d(1LL << 32, wx, wy);
}

void decode_hilbert(unsigned long long index, unsigned *wx, unsigned *wy) {
	hilbert_d2xy(1LL << 32, index, wx, wy);
}

unsigned long long encode_quadkey(unsigned int wx, unsigned int wy) {
	unsigned long long out = 0;

	int i;
	for (i = 0; i < 32; i++) {
		unsigned long long v = ((wx >> (32 - (i + 1))) & 1) << 1;
		v |= (wy >> (32 - (i + 1))) & 1;
		v = v << (64 - 2 * (i + 1));

		out |= v;
	}

	return out;
}

static std::atomic<unsigned char> decodex[256];
static std::atomic<unsigned char> decodey[256];

void decode_quadkey(unsigned long long index, unsigned *wx, unsigned *wy) {
	static std::atomic<int> initialized(0);
	if (!initialized) {
		for (size_t ix = 0; ix < 256; ix++) {
			size_t xx = 0, yy = 0;

			for (size_t i = 0; i < 32; i++) {
				xx |= ((ix >> (64 - 2 * (i + 1) + 1)) & 1) << (32 - (i + 1));
				yy |= ((ix >> (64 - 2 * (i + 1) + 0)) & 1) << (32 - (i + 1));
			}

			decodex[ix] = xx;
			decodey[ix] = yy;
		}

		initialized = 1;
	}

	*wx = *wy = 0;

	for (size_t i = 0; i < 8; i++) {
		*wx |= ((unsigned) decodex[(index >> (8 * i)) & 0xFF]) << (4 * i);
		*wy |= ((unsigned) decodey[(index >> (8 * i)) & 0xFF]) << (4 * i);
	}
}

struct projection *find_projection(const char *name) {
	struct projection *p;
	for (p = projections; p->name != NULL; p++) {
		if (strcmp(p->name, name) == 0) {
			return p;
		}
		if (strcmp(p->alias, name) == 0) {
			return p;
		}
	}
	return NULL;
}

void set_projection_or_exit(const char *optarg) {
	struct projection *p = find_projection(optarg);
	if (p == NULL) {
		fprintf(stderr, "Unknown projection (-s): %s\n", optarg);
		exit(EXIT_ARGS);
	}
	projection = p;
}

static struct projection *output_projection = NULL;  // NULL: follow -s
static bool explicit_output = false;
static bool explicit_output_linear = false;

struct projection *get_output_projection(void) {
	return output_projection != NULL ? output_projection : projection;
}

void set_output_projection_or_exit(const char *optarg) {
	struct projection *p = find_projection(optarg);
	if (p == NULL) {
		fprintf(stderr, "Unknown projection (--output-projection): %s\n", optarg);
		exit(EXIT_ARGS);
	}
	output_projection = p;
	explicit_output = true;
	// An explicit non-3857 output projection means the equirectangular
	// (EPSG:4490-style) tile grid, matching common CGCS2000/WGS84 tiling.
	explicit_output_linear = (strcmp(p->name, "EPSG:3857") != 0);
}

bool output_grid_is_linear(void) {
	if (explicit_output) {
		return explicit_output_linear;
	}
	// Default: the output grid follows the input projection. Only EPSG:4490
	// input defaults to the equirectangular grid; 4326/3857 keep Web Mercator.
	return projection->linear_grid;
}

static void epsg3857_to_lonlat(double x, double y, double *lon, double *lat) {
	*lon = x * 180.0 / (M_PI * 6378137.0);
	*lat = atan(sinh(y / 6378137.0)) * 180.0 / M_PI;
}

void project_input(double ix, double iy, int zoom, long long *ox, long long *oy) {
	if (projection->geographic) {
		if (output_grid_is_linear()) {
			epsg4490totile(ix, iy, zoom, ox, oy);
		} else {
			lonlat2tile(ix, iy, zoom, ox, oy);
		}
	} else {
		if (output_grid_is_linear()) {
			double lon, lat;
			epsg3857_to_lonlat(ix, iy, &lon, &lat);
			epsg4490totile(lon, lat, zoom, ox, oy);
		} else {
			epsg3857totile(ix, iy, zoom, ox, oy);
		}
	}
}

void unproject_output_ll(long long ix, long long iy, int zoom, double *ox, double *oy) {
	if (output_grid_is_linear()) {
		tiletoepsg4490(ix, iy, zoom, ox, oy);
	} else {
		tile2lonlat(ix, iy, zoom, ox, oy);
	}
}

bool output_tile_matrix_meta(const char **crs, double *origin_x, double *origin_y, double *z0_dim) {
	if (!output_grid_is_linear()) {
		return false;
	}
	struct projection *out = get_output_projection();
	*crs = out->name;
	*origin_x = -180.0;
	*origin_y = 90.0;
	*z0_dim = 360.0;
	return true;
}

unsigned long long encode_vertex(unsigned int wx, unsigned int wy) {
	return (((unsigned long long) wx) << 32) | wy;
}
