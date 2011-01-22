/*
 * Geeqie
 * (C) 2006 John Ellis
 * Copyright (C) 2008 - 2010 The Geeqie Team
 *
 * Author: John Ellis
 *
 * This software is released under the GNU General Public License (GNU GPL).
 * Please read the included file COPYING for more information.
 * This software comes with no warranty of any kind, use at your own risk!
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "pixbuf-renderer.h"

#include "intl.h"
#include "layout.h"

#include <gtk/gtk.h>


/* comment this out if not using this from within Geeqie
 * defining GQ_BUILD does these things:
 *   - Sets the shift-click scroller pixbuf to a nice icon instead of a black box
 */
#define GQ_BUILD 1

#ifdef GQ_BUILD
#include "main.h"
#include "pixbuf_util.h"
#include "exif.h"
#else
typedef enum {
	EXIF_ORIENTATION_UNKNOWN	= 0,
	EXIF_ORIENTATION_TOP_LEFT	= 1,
	EXIF_ORIENTATION_TOP_RIGHT	= 2,
	EXIF_ORIENTATION_BOTTOM_RIGHT	= 3,
	EXIF_ORIENTATION_BOTTOM_LEFT	= 4,
	EXIF_ORIENTATION_LEFT_TOP	= 5,
	EXIF_ORIENTATION_RIGHT_TOP	= 6,
	EXIF_ORIENTATION_RIGHT_BOTTOM	= 7,
	EXIF_ORIENTATION_LEFT_BOTTOM	= 8
} ExifOrientationType;
#endif


/* size to use when breaking up image pane for rendering */
#define PR_TILE_SIZE 128

/* default size of tile cache (mb) */
#define PR_CACHE_SIZE_DEFAULT 8

/* default min and max zoom */
#define PR_ZOOM_MIN -32.0
#define PR_ZOOM_MAX 32.0

/* distance to drag mouse to disable image flip */
#define PR_DRAG_SCROLL_THRESHHOLD 4

/* increase pan rate when holding down shift */
#define PR_PAN_SHIFT_MULTIPLIER 6

/* scroller config */
#define PR_SCROLLER_UPDATES_PER_SEC 30
#define PR_SCROLLER_DEAD_ZONE 6

/* alpha channel checkerboard background (same as gimp) */
#define PR_ALPHA_CHECK1 0x00999999
#define PR_ALPHA_CHECK2 0x00666666
#define PR_ALPHA_CHECK_SIZE 16

/* when scaling image to below this size, use nearest pixel for scaling
 * (below about 4, the other scale types become slow generating their conversion tables)
 */
#define PR_MIN_SCALE_SIZE 8

/* round A up/down to integer count of B */
#define ROUND_UP(A,B)   ((gint)(((A)+(B)-1)/(B))*(B))
#define ROUND_DOWN(A,B) ((gint)(((A))/(B))*(B))

typedef enum {
	TILE_RENDER_NONE = 0,	/* do nothing */
	TILE_RENDER_AREA,	/* render an area of the tile */
	TILE_RENDER_ALL		/* render the whole tile */
} ImageTileRenderType;

typedef struct _ImageTile ImageTile;
typedef struct _QueueData QueueData;

struct _ImageTile
{
	GdkPixmap *pixmap;	/* off screen buffer */
	GdkPixbuf *pixbuf;	/* pixbuf area for zooming */
	gint x;			/* x offset into image */
	gint y;			/* y offset into image */
	gint w;			/* width that is visible (may be less if at edge of image) */
	gint h;			/* height '' */

	gboolean blank;

/* render_todo: (explanation)
	NONE	do nothing
	AREA	render area of tile, usually only used when loading an image
		note: will jump to an ALL if render_done is not ALL.
	ALL	render entire tile, if never done before w/ ALL, for expose events *only*
*/

	ImageTileRenderType render_todo;	/* what to do (see above) */
	ImageTileRenderType render_done;	/* highest that has been done before on tile */

	QueueData *qd;
	QueueData *qd2;

	guint size;		/* est. memory used by pixmap and pixbuf */
};

struct _QueueData
{
	ImageTile *it;
	gint x;
	gint y;
	gint w;
	gint h;
	gboolean new_data;
};

typedef struct _SourceTile SourceTile;
struct _SourceTile
{
	gint x;
	gint y;
	GdkPixbuf *pixbuf;
	gboolean blank;
};

typedef struct _OverlayData OverlayData;
struct _OverlayData
{
	gint id;

	GdkPixbuf *pixbuf;
	GdkWindow *window;

	gint x;
	gint y;

	OverlayRendererFlags flags;
};

enum {
	SIGNAL_ZOOM = 0,
	SIGNAL_CLICKED,
	SIGNAL_SCROLL_NOTIFY,
	SIGNAL_RENDER_COMPLETE,
	SIGNAL_DRAG,
	SIGNAL_UPDATE_PIXEL,
	SIGNAL_COUNT
};

enum {
	PROP_0,
	PROP_ZOOM_MIN,
	PROP_ZOOM_MAX,
	PROP_ZOOM_QUALITY,
	PROP_ZOOM_2PASS,
	PROP_ZOOM_EXPAND,
	PROP_DITHER_QUALITY,
	PROP_SCROLL_RESET,
	PROP_DELAY_FLIP,
	PROP_LOADING,
	PROP_COMPLETE,
	PROP_CACHE_SIZE_DISPLAY,
	PROP_CACHE_SIZE_TILES,
	PROP_WINDOW_FIT,
	PROP_WINDOW_LIMIT,
	PROP_WINDOW_LIMIT_VALUE,
	PROP_AUTOFIT_LIMIT,
	PROP_AUTOFIT_LIMIT_VALUE
};

typedef enum {
	PR_ZOOM_NONE		= 0,
	PR_ZOOM_FORCE 		= 1 << 0,
	PR_ZOOM_NEW		= 1 << 1,
	PR_ZOOM_CENTER		= 1 << 2,
	PR_ZOOM_INVALIDATE	= 1 << 3,
	PR_ZOOM_LAZY		= 1 << 4  /* wait with redraw for pixbuf_renderer_area_changed */
} PrZoomFlags;

static guint signals[SIGNAL_COUNT] = { 0 };
static GtkEventBoxClass *parent_class = NULL;



static void pixbuf_renderer_class_init(PixbufRendererClass *class);
static void pixbuf_renderer_init(PixbufRenderer *pr);
static void pixbuf_renderer_finalize(GObject *object);
static void pixbuf_renderer_set_property(GObject *object, guint prop_id,
					 const GValue *value, GParamSpec *pspec);
static void pixbuf_renderer_get_property(GObject *object, guint prop_id,
					 GValue *value, GParamSpec *pspec);
static gboolean pixbuf_renderer_expose(GtkWidget *widget, GdkEventExpose *event);

static void pr_render_complete_signal(PixbufRenderer *pr);

static void pr_overlay_list_clear(PixbufRenderer *pr);
static void pr_scroller_timer_set(PixbufRenderer *pr, gboolean start);
static void pr_border_draw(PixbufRenderer *pr, gint x, gint y, gint w, gint h);


static void pr_source_tile_free_all(PixbufRenderer *pr);
static void pr_tile_free_all(PixbufRenderer *pr);
static void pr_tile_invalidate_region(PixbufRenderer *pr, gint x, gint y, gint w, gint h);
static gboolean pr_tile_is_visible(PixbufRenderer *pr, ImageTile *it);
static void pr_queue_clear(PixbufRenderer *pr);
static void pr_queue_merge(QueueData *parent, QueueData *qd);
static void pr_queue(PixbufRenderer *pr, gint x, gint y, gint w, gint h,
		     gint clamp, ImageTileRenderType render, gboolean new_data, gboolean only_existing);

static void pr_redraw(PixbufRenderer *pr, gboolean new_data);

static void pr_zoom_sync(PixbufRenderer *pr, gdouble zoom,
			 PrZoomFlags flags, gint px, gint py);

static void pr_signals_connect(PixbufRenderer *pr);
static void pr_size_cb(GtkWidget *widget, GtkAllocation *allocation, gpointer data);
static void pr_hierarchy_changed_cb(GtkWidget *widget, GtkWidget *previous_toplevel, gpointer data);
static void pixbuf_renderer_paint(PixbufRenderer *pr, GdkRectangle *area);
static gint pr_queue_draw_idle_cb(gpointer data);


/*
 *-------------------------------------------------------------------
 * Pixbuf Renderer object
 *-------------------------------------------------------------------
 */

GType pixbuf_renderer_get_type(void)
{
	static GType pixbuf_renderer_type = 0;

	if (!pixbuf_renderer_type)
		{
		static const GTypeInfo pixbuf_renderer_info =
			{
			sizeof(PixbufRendererClass), /* class_size */
			NULL,		/* base_init */
			NULL,		/* base_finalize */
			(GClassInitFunc)pixbuf_renderer_class_init,
			NULL,		/* class_finalize */
			NULL,		/* class_data */
			sizeof(PixbufRenderer), /* instance_size */
			0,		/* n_preallocs */
			(GInstanceInitFunc)pixbuf_renderer_init, /* instance_init */
			NULL,		/* value_table */
			};

		pixbuf_renderer_type = g_type_register_static(GTK_TYPE_EVENT_BOX, "PixbufRenderer",
							      &pixbuf_renderer_info, 0);
		}

	return pixbuf_renderer_type;
}

static void pixbuf_renderer_class_init(PixbufRendererClass *class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(class);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);

	parent_class = g_type_class_peek_parent(class);

	gobject_class->set_property = pixbuf_renderer_set_property;
	gobject_class->get_property = pixbuf_renderer_get_property;

	gobject_class->finalize = pixbuf_renderer_finalize;

	widget_class->expose_event = pixbuf_renderer_expose;

	g_object_class_install_property(gobject_class,
					PROP_ZOOM_MIN,
					g_param_spec_double("zoom_min",
							    "Zoom minimum",
							    NULL,
							    -1000.0,
							    1000.0,
							    PR_ZOOM_MIN,
							    G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_ZOOM_MAX,
					g_param_spec_double("zoom_max",
							    "Zoom maximum",
							    NULL,
							    -1000.0,
							    1000.0,
							    PR_ZOOM_MIN,
							    G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_ZOOM_QUALITY,
					g_param_spec_uint("zoom_quality",
							  "Zoom quality",
							  NULL,
							  GDK_INTERP_NEAREST,
							  GDK_INTERP_HYPER,
							  GDK_INTERP_BILINEAR,
							  G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_ZOOM_2PASS,
					g_param_spec_boolean("zoom_2pass",
							     "2 pass zoom",
							     NULL,
							     TRUE,
							     G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_ZOOM_EXPAND,
					g_param_spec_boolean("zoom_expand",
							     "Expand image in autozoom.",
							     NULL,
							     FALSE,
							     G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_DITHER_QUALITY,
					g_param_spec_uint("dither_quality",
							  "Dither quality",
							  NULL,
							  GDK_RGB_DITHER_NONE,
							  GDK_RGB_DITHER_MAX,
							  GDK_RGB_DITHER_NORMAL,
							  G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_SCROLL_RESET,
					g_param_spec_uint("scroll_reset",
							  "New image scroll reset",
							  NULL,
							  PR_SCROLL_RESET_TOPLEFT,
							  PR_SCROLL_RESET_NOCHANGE,
							  PR_SCROLL_RESET_TOPLEFT,
							  G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_DELAY_FLIP,
					g_param_spec_boolean("delay_flip",
							     "Delay image update",
							     NULL,
							     FALSE,
							     G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_LOADING,
					g_param_spec_boolean("loading",
							     "Image actively loading",
							     NULL,
							     FALSE,
							     G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_COMPLETE,
					g_param_spec_boolean("complete",
							     "Image rendering complete",
							     NULL,
							     FALSE,
							     G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_CACHE_SIZE_DISPLAY,
					g_param_spec_uint("cache_display",
							  "Display cache size MB",
							  NULL,
							  0,
							  128,
							  PR_CACHE_SIZE_DEFAULT,
							  G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_CACHE_SIZE_TILES,
					g_param_spec_uint("cache_tiles",
							  "Tile cache count",
							  "Number of tiles to retain in memory at any one time.",
							  0,
							  256,
							  PR_CACHE_SIZE_DEFAULT,
							  G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_WINDOW_FIT,
					g_param_spec_boolean("window_fit",
							     "Fit window to image size",
							     NULL,
							     FALSE,
							     G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_WINDOW_LIMIT,
					g_param_spec_boolean("window_limit",
							     "Limit size of parent window",
							     NULL,
							     FALSE,
							     G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_WINDOW_LIMIT_VALUE,
					g_param_spec_uint("window_limit_value",
							  "Size limit of parent window",
							  NULL,
							  10,
							  150,
							  100,
							  G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_AUTOFIT_LIMIT,
					g_param_spec_boolean("autofit_limit",
							     "Limit size of image when autofitting",
							     NULL,
							     FALSE,
							     G_PARAM_READABLE | G_PARAM_WRITABLE));

	g_object_class_install_property(gobject_class,
					PROP_AUTOFIT_LIMIT_VALUE,
					g_param_spec_uint("autofit_limit_value",
							  "Size limit of image when autofitting",
							  NULL,
							  10,
							  150,
							  100,
							  G_PARAM_READABLE | G_PARAM_WRITABLE));


	signals[SIGNAL_ZOOM] =
		g_signal_new("zoom",
			     G_OBJECT_CLASS_TYPE(gobject_class),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(PixbufRendererClass, zoom),
			     NULL, NULL,
			     g_cclosure_marshal_VOID__DOUBLE,
			     G_TYPE_NONE, 1,
			     G_TYPE_DOUBLE);

	signals[SIGNAL_CLICKED] =
		g_signal_new("clicked",
			     G_OBJECT_CLASS_TYPE(gobject_class),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(PixbufRendererClass, clicked),
			     NULL, NULL,
			     g_cclosure_marshal_VOID__BOXED,
			     G_TYPE_NONE, 1,
			     GDK_TYPE_EVENT);

	signals[SIGNAL_SCROLL_NOTIFY] =
		g_signal_new("scroll-notify",
			     G_OBJECT_CLASS_TYPE(gobject_class),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(PixbufRendererClass, scroll_notify),
			     NULL, NULL,
			     g_cclosure_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);

	signals[SIGNAL_RENDER_COMPLETE] =
		g_signal_new("render-complete",
			     G_OBJECT_CLASS_TYPE(gobject_class),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(PixbufRendererClass, render_complete),
			     NULL, NULL,
			     g_cclosure_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);

	signals[SIGNAL_DRAG] =
		g_signal_new("drag",
			     G_OBJECT_CLASS_TYPE(gobject_class),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(PixbufRendererClass, drag),
			     NULL, NULL,
			     g_cclosure_marshal_VOID__BOXED,
			     G_TYPE_NONE, 1,
			     GDK_TYPE_EVENT);
			     
	signals[SIGNAL_UPDATE_PIXEL] =
		g_signal_new("update-pixel",
			     G_OBJECT_CLASS_TYPE(gobject_class),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(PixbufRendererClass, update_pixel),
			     NULL, NULL,
			     g_cclosure_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
}

static void pixbuf_renderer_init(PixbufRenderer *pr)
{
	GtkWidget *box;

	box = GTK_WIDGET(pr);

	pr->zoom_min = PR_ZOOM_MIN;
	pr->zoom_max = PR_ZOOM_MAX;
	pr->zoom_quality = GDK_INTERP_BILINEAR;
	pr->zoom_2pass = FALSE;

	pr->zoom = 1.0;
	pr->scale = 1.0;

	pr->dither_quality = GDK_RGB_DITHER_NORMAL;

	pr->scroll_reset = PR_SCROLL_RESET_TOPLEFT;

	pr->draw_idle_id = 0;

	pr->tile_width = PR_TILE_SIZE;
	pr->tile_height = PR_TILE_SIZE;

	pr->tiles = NULL;
	pr->tile_cache_size = 0;

	pr->tile_cache_max = PR_CACHE_SIZE_DEFAULT;

	pr->scroller_id = 0;
	pr->scroller_overlay = -1;
	
	pr->x_mouse = -1;
	pr->y_mouse = -1;

	pr->source_tiles_enabled = FALSE;
	pr->source_tiles = NULL;

	pr->orientation = 1;

	pr->norm_center_x = 0.5;
	pr->norm_center_y = 0.5;

	gtk_widget_set_double_buffered(box, FALSE);
	g_signal_connect_after(G_OBJECT(box), "size_allocate",
			       G_CALLBACK(pr_size_cb), pr);

	g_signal_connect(G_OBJECT(pr), "hierarchy-changed",
			 G_CALLBACK(pr_hierarchy_changed_cb), pr);

	pr_signals_connect(pr);
}

static void pixbuf_renderer_finalize(GObject *object)
{
	PixbufRenderer *pr;

	pr = PIXBUF_RENDERER(object);

	pr_queue_clear(pr);
	pr_tile_free_all(pr);


	if (pr->pixbuf) g_object_unref(pr->pixbuf);
	if (pr->spare_tile) g_object_unref(pr->spare_tile);

	pr_scroller_timer_set(pr, FALSE);
	pr_overlay_list_clear(pr);

	pr_source_tile_free_all(pr);
}

PixbufRenderer *pixbuf_renderer_new(void)
{
	return g_object_new(TYPE_PIXBUF_RENDERER, NULL);
}

static void pixbuf_renderer_set_property(GObject *object, guint prop_id,
					 const GValue *value, GParamSpec *pspec)
{
	PixbufRenderer *pr;

	pr = PIXBUF_RENDERER(object);

	switch (prop_id)
		{
		case PROP_ZOOM_MIN:
			pr->zoom_min = g_value_get_double(value);
			break;
		case PROP_ZOOM_MAX:
			pr->zoom_max = g_value_get_double(value);
			break;
		case PROP_ZOOM_QUALITY:
			pr->zoom_quality = g_value_get_uint(value);
			break;
		case PROP_ZOOM_2PASS:
			pr->zoom_2pass = g_value_get_boolean(value);
			break;
		case PROP_ZOOM_EXPAND:
			pr->zoom_expand = g_value_get_boolean(value);
			break;
		case PROP_DITHER_QUALITY:
			pr->dither_quality = g_value_get_uint(value);
			break;
		case PROP_SCROLL_RESET:
			pr->scroll_reset = g_value_get_uint(value);
			break;
		case PROP_DELAY_FLIP:
			pr->delay_flip = g_value_get_boolean(value);
			break;
		case PROP_LOADING:
			pr->loading = g_value_get_boolean(value);
			break;
		case PROP_COMPLETE:
			pr->complete = g_value_get_boolean(value);
			break;
		case PROP_CACHE_SIZE_DISPLAY:
			pr->tile_cache_max = g_value_get_uint(value);
			break;
		case PROP_CACHE_SIZE_TILES:
			pr->source_tiles_cache_size = g_value_get_uint(value);
			break;
		case PROP_WINDOW_FIT:
			pr->window_fit = g_value_get_boolean(value);
			break;
		case PROP_WINDOW_LIMIT:
			pr->window_limit = g_value_get_boolean(value);
			break;
		case PROP_WINDOW_LIMIT_VALUE:
			pr->window_limit_size = g_value_get_uint(value);
			break;
		case PROP_AUTOFIT_LIMIT:
			pr->autofit_limit = g_value_get_boolean(value);
			break;
		case PROP_AUTOFIT_LIMIT_VALUE:
			pr->autofit_limit_size = g_value_get_uint(value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
		}
}

static void pixbuf_renderer_get_property(GObject *object, guint prop_id,
					 GValue *value, GParamSpec *pspec)
{
	PixbufRenderer *pr;

	pr = PIXBUF_RENDERER(object);

	switch (prop_id)
		{
		case PROP_ZOOM_MIN:
			g_value_set_double(value, pr->zoom_min);
			break;
		case PROP_ZOOM_MAX:
			g_value_set_double(value, pr->zoom_max);
			break;
		case PROP_ZOOM_QUALITY:
			g_value_set_uint(value, pr->zoom_quality);
			break;
		case PROP_ZOOM_2PASS:
			g_value_set_boolean(value, pr->zoom_2pass);
			break;
		case PROP_ZOOM_EXPAND:
			g_value_set_boolean(value, pr->zoom_expand);
			break;
		case PROP_DITHER_QUALITY:
			g_value_set_uint(value, pr->dither_quality);
			break;
		case PROP_SCROLL_RESET:
			g_value_set_uint(value, pr->scroll_reset);
			break;
		case PROP_DELAY_FLIP:
			g_value_set_boolean(value, pr->delay_flip);
			break;
		case PROP_LOADING:
			g_value_set_boolean(value, pr->loading);
			break;
		case PROP_COMPLETE:
			g_value_set_boolean(value, pr->complete);
			break;
		case PROP_CACHE_SIZE_DISPLAY:
			g_value_set_uint(value, pr->tile_cache_max);
			break;
		case PROP_CACHE_SIZE_TILES:
			g_value_set_uint(value, pr->source_tiles_cache_size);
			break;
		case PROP_WINDOW_FIT:
			g_value_set_boolean(value, pr->window_fit);
			break;
		case PROP_WINDOW_LIMIT:
			g_value_set_boolean(value, pr->window_limit);
			break;
		case PROP_WINDOW_LIMIT_VALUE:
			g_value_set_uint(value, pr->window_limit_size);
			break;
		case PROP_AUTOFIT_LIMIT:
			g_value_set_boolean(value, pr->autofit_limit);
			break;
		case PROP_AUTOFIT_LIMIT_VALUE:
			g_value_set_uint(value, pr->autofit_limit_size);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
		}
}

static gboolean pixbuf_renderer_expose(GtkWidget *widget, GdkEventExpose *event)
{
#if GTK_CHECK_VERSION(2,20,0)
	if (gtk_widget_is_drawable(widget))
#else
	if (GTK_WIDGET_DRAWABLE(widget))
#endif
		{
#if GTK_CHECK_VERSION(2,20,0)
		if (gtk_widget_get_has_window(widget))
#else
		if (!GTK_WIDGET_NO_WINDOW(widget))
#endif
			{
			if (event->window != widget->window)
				{
				GdkRectangle area;

				gdk_window_get_position(event->window, &area.x, &area.y);

				area.x += event->area.x;
				area.y += event->area.y;
				area.width = event->area.width;
				area.height = event->area.height;
				pixbuf_renderer_paint(PIXBUF_RENDERER(widget), &area);
				}
			else
				{
				pixbuf_renderer_paint(PIXBUF_RENDERER(widget), &event->area);
				}
			}
		}

	return FALSE;
}

/*
 *-------------------------------------------------------------------
 * misc utilities
 *-------------------------------------------------------------------
 */

static void widget_set_cursor(GtkWidget *widget, gint icon)
{
	GdkCursor *cursor;

	if (!widget->window) return;

	if (icon == -1)
		{
		cursor = NULL;
		}
	else
		{
		cursor = gdk_cursor_new(icon);
		}

	gdk_window_set_cursor(widget->window, cursor);

	if (cursor) gdk_cursor_unref(cursor);
}

static gint pixmap_calc_size(GdkPixmap *pixmap)
{
	gint w, h, d;

	d = gdk_drawable_get_depth(pixmap);
	gdk_drawable_get_size(pixmap, &w, &h);
	return w * h * (d / 8);
}

static gboolean pr_clip_region(gint x, gint y, gint w, gint h,
			       gint clip_x, gint clip_y, gint clip_w, gint clip_h,
			       gint *rx, gint *ry, gint *rw, gint *rh)
{
	if (clip_x + clip_w <= x ||
	    clip_x >= x + w ||
	    clip_y + clip_h <= y ||
	    clip_y >= y + h)
		{
		return FALSE;
		}

	*rx = MAX(x, clip_x);
	*rw = MIN((x + w), (clip_x + clip_w)) - *rx;

	*ry = MAX(y, clip_y);
	*rh = MIN((y + h), (clip_y + clip_h)) - *ry;

	return TRUE;
}

static gboolean pr_parent_window_sizable(PixbufRenderer *pr)
{
	GdkWindowState state;

	if (!pr->parent_window) return FALSE;
	if (!pr->window_fit) return FALSE;
	if (!GTK_WIDGET(pr)->window) return FALSE;

	if (!pr->parent_window->window) return FALSE;
	state = gdk_window_get_state(pr->parent_window->window);
	if (state & GDK_WINDOW_STATE_MAXIMIZED) return FALSE;

	return TRUE;
}

static gboolean pr_parent_window_resize(PixbufRenderer *pr, gint w, gint h)
{
	GtkWidget *widget;
	GtkWidget *parent;
	gint ww, wh;

	if (!pr_parent_window_sizable(pr)) return FALSE;

	if (pr->window_limit)
		{
		gint sw = gdk_screen_width() * pr->window_limit_size / 100;
		gint sh = gdk_screen_height() * pr->window_limit_size / 100;

		if (w > sw) w = sw;
		if (h > sh) h = sh;
		}

	widget = GTK_WIDGET(pr);
	parent = GTK_WIDGET(pr->parent_window);

	w += (parent->allocation.width - widget->allocation.width);
	h += (parent->allocation.height - widget->allocation.height);

	gdk_drawable_get_size(parent->window, &ww, &wh);
	if (w == ww && h == wh) return FALSE;

	gdk_window_resize(parent->window, w, h);

	return TRUE;
}

void pixbuf_renderer_set_parent(PixbufRenderer *pr, GtkWindow *window)
{
	g_return_if_fail(IS_PIXBUF_RENDERER(pr));
	g_return_if_fail(window == NULL || GTK_IS_WINDOW(window));

	pr->parent_window = GTK_WIDGET(window);
}

GtkWindow *pixbuf_renderer_get_parent(PixbufRenderer *pr)
{
	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), NULL);

	return GTK_WINDOW(pr->parent_window);
}


/*
 *-------------------------------------------------------------------
 * overlays
 *-------------------------------------------------------------------
 */

static void pr_overlay_get_position(PixbufRenderer *pr, OverlayData *od,
				    gint *x, gint *y, gint *w, gint *h)
{
	gint px, py, pw, ph;

	pw = gdk_pixbuf_get_width(od->pixbuf);
	ph = gdk_pixbuf_get_height(od->pixbuf);
	px = od->x;
	py = od->y;

	if (od->flags & OVL_RELATIVE)
		{
		if (px < 0) px = pr->window_width - pw + px;
		if (py < 0) py = pr->window_height - ph + py;
		}

	if (x) *x = px;
	if (y) *y = py;
	if (w) *w = pw;
	if (h) *h = ph;
}

static void pr_overlay_init_window(PixbufRenderer *pr, OverlayData *od)
{
	gint px, py, pw, ph;
	GdkWindowAttr attributes;
	gint attributes_mask;

	pr_overlay_get_position(pr, od, &px, &py, &pw, &ph);

	attributes.window_type = GDK_WINDOW_CHILD;
	attributes.wclass = GDK_INPUT_OUTPUT;
	attributes.width = pw;
	attributes.height = ph;
	attributes.event_mask = GDK_EXPOSURE_MASK;
	attributes_mask = 0;

	od->window = gdk_window_new(GTK_WIDGET(pr)->window, &attributes, attributes_mask);
	gdk_window_set_user_data(od->window, pr);
	gdk_window_move(od->window, px, py);
	gdk_window_show(od->window);
}

static void pr_overlay_draw(PixbufRenderer *pr, gint x, gint y, gint w, gint h,
			    ImageTile *it)
{
	GtkWidget *box;
	GList *work;

	box = GTK_WIDGET(pr);

	work = pr->overlay_list;
	while (work)
		{
		OverlayData *od;
		gint px, py, pw, ph;
		gint rx, ry, rw, rh;

		od = work->data;
		work = work->next;

		if (!od->window) pr_overlay_init_window(pr, od);
		
		pr_overlay_get_position(pr, od, &px, &py, &pw, &ph);
		if (pr_clip_region(x, y, w, h, px, py, pw, ph, &rx, &ry, &rw, &rh))
			{
			if (!pr->overlay_buffer)
				{
				pr->overlay_buffer = gdk_pixmap_new(((GtkWidget *)pr)->window, pr->tile_width, pr->tile_height, -1);
				}

			if (it)
				{
#if GTK_CHECK_VERSION(2,20,0)
				gdk_draw_drawable(pr->overlay_buffer, box->style->fg_gc[gtk_widget_get_state(box)],
#else
				gdk_draw_drawable(pr->overlay_buffer, box->style->fg_gc[GTK_WIDGET_STATE(box)],
#endif
						  it->pixmap,
						  rx - (pr->x_offset + (it->x - pr->x_scroll)),
						  ry - (pr->y_offset + (it->y - pr->y_scroll)),
						  0, 0, rw, rh);
				gdk_draw_pixbuf(pr->overlay_buffer,
#if GTK_CHECK_VERSION(2,20,0)
						box->style->fg_gc[gtk_widget_get_state(box)],
#else
						box->style->fg_gc[GTK_WIDGET_STATE(box)],
#endif
						od->pixbuf,
						rx - px, ry - py,
						0, 0, rw, rh,
						pr->dither_quality, rx, ry);
#if GTK_CHECK_VERSION(2,20,0)
				gdk_draw_drawable(od->window, box->style->fg_gc[gtk_widget_get_state(box)],
#else
				gdk_draw_drawable(od->window, box->style->fg_gc[GTK_WIDGET_STATE(box)],
#endif
						  pr->overlay_buffer,
						  0, 0,
						  rx - px, ry - py, rw, rh);
				}
			else
				{
				/* no ImageTile means region may be larger than our scratch buffer */
				gint sx, sy;

				for (sx = rx; sx < rx + rw; sx += pr->tile_width)
				    for (sy = ry; sy < ry + rh; sy += pr->tile_height)
					{
					gint sw, sh;

					sw = MIN(rx + rw - sx, pr->tile_width);
					sh = MIN(ry + rh - sy, pr->tile_height);

					gdk_draw_rectangle(pr->overlay_buffer,
#if GTK_CHECK_VERSION(2,20,0)
							   box->style->bg_gc[gtk_widget_get_state(box)], TRUE,
#else
							   box->style->bg_gc[GTK_WIDGET_STATE(box)], TRUE,
#endif
							   0, 0, sw, sh);
					gdk_draw_pixbuf(pr->overlay_buffer,
#if GTK_CHECK_VERSION(2,20,0)
							box->style->fg_gc[gtk_widget_get_state(box)],
#else
							box->style->fg_gc[GTK_WIDGET_STATE(box)],
#endif
							od->pixbuf,
							sx - px, sy - py,
							0, 0, sw, sh,
							pr->dither_quality, sx, sy);
#if GTK_CHECK_VERSION(2,20,0)
					gdk_draw_drawable(od->window, box->style->fg_gc[gtk_widget_get_state(box)],
#else
					gdk_draw_drawable(od->window, box->style->fg_gc[GTK_WIDGET_STATE(box)],
#endif
							  pr->overlay_buffer,
							  0, 0,
							  sx - px, sy - py, sw, sh);
					}
				}
			}
		}
}

static void pr_overlay_queue_draw(PixbufRenderer *pr, OverlayData *od)
{
	gint x, y, w, h;

	pr_overlay_get_position(pr, od, &x, &y, &w, &h);
	pr_queue(pr, pr->x_scroll - pr->x_offset + x,
		 pr->y_scroll - pr->y_offset + y,
		 w, h,
		 FALSE, TILE_RENDER_ALL, FALSE, FALSE);

	pr_border_draw(pr, x, y, w, h);
}

static void pr_overlay_queue_all(PixbufRenderer *pr)
{
	GList *work;

	work = pr->overlay_list;
	while (work)
		{
		OverlayData *od = work->data;
		work = work->next;

		pr_overlay_queue_draw(pr, od);
		}
}

static void pr_overlay_update_sizes(PixbufRenderer *pr)
{
	GList *work;

	work = pr->overlay_list;
	while (work)
		{
		OverlayData *od = work->data;
		work = work->next;
		
		if (!od->window) pr_overlay_init_window(pr, od);
		
		if (od->flags & OVL_RELATIVE)
			{
			gint x, y, w, h;

			pr_overlay_get_position(pr, od, &x, &y, &w, &h);
			gdk_window_move_resize(od->window, x, y, w, h);
			}
		}
}

static OverlayData *pr_overlay_find(PixbufRenderer *pr, gint id)
{
	GList *work;

	work = pr->overlay_list;
	while (work)
		{
		OverlayData *od = work->data;
		work = work->next;

		if (od->id == id) return od;
		}

	return NULL;
}


gint pixbuf_renderer_overlay_add(PixbufRenderer *pr, GdkPixbuf *pixbuf, gint x, gint y,
				 OverlayRendererFlags flags)
{
	OverlayData *od;
	gint id;

	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), -1);
	g_return_val_if_fail(pixbuf != NULL, -1);

	id = 1;
	while (pr_overlay_find(pr, id)) id++;

	od = g_new0(OverlayData, 1);
	od->id = id;
	od->pixbuf = pixbuf;
	g_object_ref(G_OBJECT(od->pixbuf));
	od->x = x;
	od->y = y;
	od->flags = flags;

	pr_overlay_init_window(pr, od);
	
	pr->overlay_list = g_list_append(pr->overlay_list, od);

	pr_overlay_queue_draw(pr, od);

	return od->id;
}

static void pr_overlay_free(PixbufRenderer *pr, OverlayData *od)
{
	pr->overlay_list = g_list_remove(pr->overlay_list, od);

	if (od->pixbuf) g_object_unref(G_OBJECT(od->pixbuf));
	if (od->window) gdk_window_destroy(od->window);
	g_free(od);

	if (!pr->overlay_list && pr->overlay_buffer)
		{
		g_object_unref(pr->overlay_buffer);
		pr->overlay_buffer = NULL;
		}
}

static void pr_overlay_list_clear(PixbufRenderer *pr)
{
	while (pr->overlay_list)
		{
		OverlayData *od;

		od = pr->overlay_list->data;
		pr_overlay_free(pr, od);
		}
}

static void pr_overlay_list_reset_window(PixbufRenderer *pr)
{
	GList *work;

	if (pr->overlay_buffer) g_object_unref(pr->overlay_buffer);
	pr->overlay_buffer = NULL;

	work = pr->overlay_list;
	while (work)
		{
		OverlayData *od = work->data;
		work = work->next;
		if (od->window) gdk_window_destroy(od->window);
		od->window = NULL;
		}
}

void pixbuf_renderer_overlay_set(PixbufRenderer *pr, gint id, GdkPixbuf *pixbuf, gint x, gint y)
{
	OverlayData *od;

	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	od = pr_overlay_find(pr, id);
	if (!od) return;

	if (pixbuf)
		{
		gint px, py, pw, ph;

		g_object_ref(G_OBJECT(pixbuf));
		g_object_unref(G_OBJECT(od->pixbuf));
		od->pixbuf = pixbuf;

		od->x = x;
		od->y = y;

		if (!od->window) pr_overlay_init_window(pr, od);

		pr_overlay_queue_draw(pr, od);
		pr_overlay_get_position(pr, od, &px, &py, &pw, &ph);
		gdk_window_move_resize(od->window, px, py, pw, ph);
		}
	else
		{
		pr_overlay_queue_draw(pr, od);
		pr_overlay_free(pr, od);
		}
}

gboolean pixbuf_renderer_overlay_get(PixbufRenderer *pr, gint id, GdkPixbuf **pixbuf, gint *x, gint *y)
{
	OverlayData *od;

	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), FALSE);

	od = pr_overlay_find(pr, id);
	if (!od) return FALSE;

	if (pixbuf) *pixbuf = od->pixbuf;
	if (x) *x = od->x;
	if (y) *y = od->y;

	return TRUE;
}

void pixbuf_renderer_overlay_remove(PixbufRenderer *pr, gint id)
{
	pixbuf_renderer_overlay_set(pr, id, NULL, 0, 0);
}

static void pr_hierarchy_changed_cb(GtkWidget *widget, GtkWidget *previous_toplevel, gpointer data)
{
	PixbufRenderer *pr = data;
	pr_overlay_list_reset_window(pr);
}


/*
 *-------------------------------------------------------------------
 * scroller overlay
 *-------------------------------------------------------------------
 */


static gboolean pr_scroller_update_cb(gpointer data)
{
	PixbufRenderer *pr = data;
	gint x, y;
	gint xinc, yinc;

	/* this was a simple scroll by difference between scroller and mouse position,
	 * but all this math results in a smoother result and accounts for a dead zone.
	 */

	if (abs(pr->scroller_xpos - pr->scroller_x) < PR_SCROLLER_DEAD_ZONE)
		{
		x = 0;
		}
	else
		{
		gint shift = PR_SCROLLER_DEAD_ZONE / 2 * PR_SCROLLER_UPDATES_PER_SEC;
		x = (pr->scroller_xpos - pr->scroller_x) / 2 * PR_SCROLLER_UPDATES_PER_SEC;
		x += (x > 0) ? -shift : shift;
		}

	if (abs(pr->scroller_ypos - pr->scroller_y) < PR_SCROLLER_DEAD_ZONE)
		{
		y = 0;
		}
	else
		{
		gint shift = PR_SCROLLER_DEAD_ZONE / 2 * PR_SCROLLER_UPDATES_PER_SEC;
		y = (pr->scroller_ypos - pr->scroller_y) / 2 * PR_SCROLLER_UPDATES_PER_SEC;
		y += (y > 0) ? -shift : shift;
		}

	if (abs(x) < PR_SCROLLER_DEAD_ZONE * PR_SCROLLER_UPDATES_PER_SEC)
		{
		xinc = x;
		}
	else
		{
		xinc = pr->scroller_xinc;

		if (x >= 0)
			{
			if (xinc < 0) xinc = 0;
			if (x < xinc) xinc = x;
			if (x > xinc) xinc = MIN(xinc + x / PR_SCROLLER_UPDATES_PER_SEC, x);
			}
		else
			{
			if (xinc > 0) xinc = 0;
			if (x > xinc) xinc = x;
			if (x < xinc) xinc = MAX(xinc + x / PR_SCROLLER_UPDATES_PER_SEC, x);
			}
		}

	if (abs(y) < PR_SCROLLER_DEAD_ZONE * PR_SCROLLER_UPDATES_PER_SEC)
		{
		yinc = y;
		}
	else
		{
		yinc = pr->scroller_yinc;

		if (y >= 0)
			{
			if (yinc < 0) yinc = 0;
			if (y < yinc) yinc = y;
			if (y > yinc) yinc = MIN(yinc + y / PR_SCROLLER_UPDATES_PER_SEC, y);
			}
		else
			{
			if (yinc > 0) yinc = 0;
			if (y > yinc) yinc = y;
			if (y < yinc) yinc = MAX(yinc + y / PR_SCROLLER_UPDATES_PER_SEC, y);
			}
		}

	pr->scroller_xinc = xinc;
	pr->scroller_yinc = yinc;

	xinc = xinc / PR_SCROLLER_UPDATES_PER_SEC;
	yinc = yinc / PR_SCROLLER_UPDATES_PER_SEC;

	pixbuf_renderer_scroll(pr, xinc, yinc);

	return TRUE;
}

static void pr_scroller_timer_set(PixbufRenderer *pr, gboolean start)
{
	if (pr->scroller_id)
		{
		g_source_remove(pr->scroller_id);
		pr->scroller_id = 0;
		}

	if (start)
		{
		pr->scroller_id = g_timeout_add(1000 / PR_SCROLLER_UPDATES_PER_SEC,
						pr_scroller_update_cb, pr);
		}
}

static void pr_scroller_start(PixbufRenderer *pr, gint x, gint y)
{
	if (pr->scroller_overlay == -1)
		{
		GdkPixbuf *pixbuf;
		gint w, h;

#ifdef GQ_BUILD
		pixbuf = pixbuf_inline(PIXBUF_INLINE_SCROLLER);
#else
		pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 32, 32);
		gdk_pixbuf_fill(pixbuf, 0x000000ff);
#endif
		w = gdk_pixbuf_get_width(pixbuf);
		h = gdk_pixbuf_get_height(pixbuf);

		pr->scroller_overlay = pixbuf_renderer_overlay_add(pr, pixbuf, x - w / 2, y - h / 2, OVL_NORMAL);
		g_object_unref(pixbuf);
		}

	pr->scroller_x = x;
	pr->scroller_y = y;
	pr->scroller_xpos = x;
	pr->scroller_ypos = y;

	pr_scroller_timer_set(pr, TRUE);
}

static void pr_scroller_stop(PixbufRenderer *pr)
{
	if (!pr->scroller_id) return;

	pixbuf_renderer_overlay_remove(pr, pr->scroller_overlay);
	pr->scroller_overlay = -1;

	pr_scroller_timer_set(pr, FALSE);
}

/*
 *-------------------------------------------------------------------
 * borders
 *-------------------------------------------------------------------
 */

static void pr_border_draw(PixbufRenderer *pr, gint x, gint y, gint w, gint h)
{
	GtkWidget *box;
	gint rx, ry, rw, rh;

	box = GTK_WIDGET(pr);

	if (!box->window) return;

	if (!pr->pixbuf && !pr->source_tiles_enabled)
		{
		if (pr_clip_region(x, y, w, h,
				   0, 0,
				   pr->window_width, pr->window_height,
				   &rx, &ry, &rw, &rh))
			{
			gdk_window_clear_area(box->window, rx, ry, rw, rh);
			pr_overlay_draw(pr, rx, ry, rw, rh, NULL);
			}
		return;
		}

	if (pr->vis_width < pr->window_width)
		{
		if (pr->x_offset > 0 &&
		    pr_clip_region(x, y, w, h,
				   0, 0,
				   pr->x_offset, pr->window_height,
				   &rx, &ry, &rw, &rh))
			{
			gdk_window_clear_area(box->window, rx, ry, rw, rh);
			pr_overlay_draw(pr, rx, ry, rw, rh, NULL);
			}
		if (pr->window_width - pr->vis_width - pr->x_offset > 0 &&
		    pr_clip_region(x, y, w, h,
				   pr->x_offset + pr->vis_width, 0,
				   pr->window_width - pr->vis_width - pr->x_offset, pr->window_height,
				   &rx, &ry, &rw, &rh))
			{
			gdk_window_clear_area(box->window, rx, ry, rw, rh);
			pr_overlay_draw(pr, rx, ry, rw, rh, NULL);
			}
		}
	if (pr->vis_height < pr->window_height)
		{
		if (pr->y_offset > 0 &&
		    pr_clip_region(x, y, w, h,
				   pr->x_offset, 0,
				   pr->vis_width, pr->y_offset,
				   &rx, &ry, &rw, &rh))
			{
			gdk_window_clear_area(box->window, rx, ry, rw, rh);
			pr_overlay_draw(pr, rx, ry, rw, rh, NULL);
			}
		if (pr->window_height - pr->vis_height - pr->y_offset > 0 &&
		    pr_clip_region(x, y, w, h,
				   pr->x_offset, pr->y_offset + pr->vis_height,
				   pr->vis_width, pr->window_height - pr->vis_height - pr->y_offset,
				   &rx, &ry, &rw, &rh))
			{
			gdk_window_clear_area(box->window, rx, ry, rw, rh);
			pr_overlay_draw(pr, rx, ry, rw, rh, NULL);
			}
		}
}

static void pr_border_clear(PixbufRenderer *pr)
{
	pr_border_draw(pr, 0, 0, pr->window_width, pr->window_height);
}

void pixbuf_renderer_set_color(PixbufRenderer *pr, GdkColor *color)
{
	GtkStyle *style;
	GtkWidget *widget;

	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	widget = GTK_WIDGET(pr);

	if (color) {
		GdkColor *slot;

		style = gtk_style_copy(gtk_widget_get_style(widget));
		slot = &style->bg[GTK_STATE_NORMAL];

		slot->red = color->red;
		slot->green = color->green;
		slot->blue = color->blue;
		}
	else {
		style = gtk_style_copy(gtk_widget_get_default_style());
	}

	gtk_widget_set_style(widget, style);

#if GTK_CHECK_VERSION(2,20,0)
	if (gtk_widget_get_visible(widget)) pr_border_clear(pr);
#else
	if (GTK_WIDGET_VISIBLE(widget)) pr_border_clear(pr);
#endif
}


/*
 *-------------------------------------------------------------------
 * source tiles
 *-------------------------------------------------------------------
 */

static void pr_source_tile_free(SourceTile *st)
{
	if (!st) return;

	if (st->pixbuf) g_object_unref(st->pixbuf);
	g_free(st);
}

static void pr_source_tile_free_all(PixbufRenderer *pr)
{
	GList *work;

	work = pr->source_tiles;
	while (work)
		{
		SourceTile *st;

		st = work->data;
		work = work->next;

		pr_source_tile_free(st);
		}

	g_list_free(pr->source_tiles);
	pr->source_tiles = NULL;
}

static void pr_source_tile_unset(PixbufRenderer *pr)
{
	pr_source_tile_free_all(pr);
	pr->source_tiles_enabled = FALSE;
}

static gboolean pr_source_tile_visible(PixbufRenderer *pr, SourceTile *st)
{
	gint x1, y1, x2, y2;

	if (!st) return FALSE;

	x1 = ROUND_DOWN(pr->x_scroll, pr->tile_width);
	y1 = ROUND_DOWN(pr->y_scroll, pr->tile_height);
	x2 = ROUND_UP(pr->x_scroll + pr->vis_width, pr->tile_width);
	y2 = ROUND_UP(pr->y_scroll + pr->vis_height, pr->tile_height);

	return !((gdouble)st->x * pr->scale > (gdouble)x2 ||
		 (gdouble)(st->x + pr->source_tile_width) * pr->scale < (gdouble)x1 ||
		 (gdouble)st->y * pr->scale > (gdouble)y2 ||
		 (gdouble)(st->y + pr->source_tile_height) * pr->scale < (gdouble)y1);
}

static SourceTile *pr_source_tile_new(PixbufRenderer *pr, gint x, gint y)
{
	SourceTile *st = NULL;
	gint count;

	g_return_val_if_fail(pr->source_tile_width >= 1 && pr->source_tile_height >= 1, NULL);

	if (pr->source_tiles_cache_size < 4) pr->source_tiles_cache_size = 4;

	count = g_list_length(pr->source_tiles);
	if (count >= pr->source_tiles_cache_size)
		{
		GList *work;

		work = g_list_last(pr->source_tiles);
		while (work && count >= pr->source_tiles_cache_size)
			{
			SourceTile *needle;

			needle = work->data;
			work = work->prev;

			if (!pr_source_tile_visible(pr, needle))
				{
				pr->source_tiles = g_list_remove(pr->source_tiles, needle);

				if (pr->func_tile_dispose)
					{
					pr->func_tile_dispose(pr, needle->x, needle->y,
							      pr->source_tile_width, pr->source_tile_height,
							      needle->pixbuf, pr->func_tile_data);
					}

				if (!st)
					{
					st = needle;
					}
				else
					{
					pr_source_tile_free(needle);
					}

				count--;
				}
			}
		}

	if (!st)
		{
		st = g_new0(SourceTile, 1);
		st->pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
					    pr->source_tile_width, pr->source_tile_height);
		}

	st->x = ROUND_DOWN(x, pr->source_tile_width);
	st->y = ROUND_DOWN(y, pr->source_tile_height);
	st->blank = TRUE;

	pr->source_tiles = g_list_prepend(pr->source_tiles, st);

	return st;
}

static SourceTile *pr_source_tile_request(PixbufRenderer *pr, gint x, gint y)
{
	SourceTile *st;

	st = pr_source_tile_new(pr, x, y);
	if (!st) return NULL;

	if (pr->func_tile_request &&
	    pr->func_tile_request(pr, st->x, st->y,
				   pr->source_tile_width, pr->source_tile_height, st->pixbuf, pr->func_tile_data))
		{
		st->blank = FALSE;
		}

	pr_tile_invalidate_region(pr, st->x * pr->scale, st->y * pr->scale,
				  pr->source_tile_width * pr->scale, pr->source_tile_height * pr->scale);

	return st;
}

static SourceTile *pr_source_tile_find(PixbufRenderer *pr, gint x, gint y)
{
	GList *work;

	work = pr->source_tiles;
	while (work)
		{
		SourceTile *st = work->data;

		if (x >= st->x && x < st->x + pr->source_tile_width &&
		    y >= st->y && y < st->y + pr->source_tile_height)
			{
			if (work != pr->source_tiles)
				{
				pr->source_tiles = g_list_remove_link(pr->source_tiles, work);
				pr->source_tiles = g_list_concat(work, pr->source_tiles);
				}
			return st;
			}

		work = work->next;
		}

	return NULL;
}

static GList *pr_source_tile_compute_region(PixbufRenderer *pr, gint x, gint y, gint w, gint h, gboolean request)
{
	gint x1, y1;
	GList *list = NULL;
	gint sx, sy;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (w > pr->image_width) w = pr->image_width;
	if (h > pr->image_height) h = pr->image_height;

	sx = ROUND_DOWN(x, pr->source_tile_width);
	sy = ROUND_DOWN(y, pr->source_tile_height);

	for (x1 = sx; x1 < x + w; x1+= pr->source_tile_width)
		{
		for (y1 = sy; y1 < y + h; y1 += pr->source_tile_height)
			{
			SourceTile *st;

			st = pr_source_tile_find(pr, x1, y1);
			if (!st && request) st = pr_source_tile_request(pr, x1, y1);

			if (st) list = g_list_prepend(list, st);
			}
		}

	return g_list_reverse(list);
}

static void pr_source_tile_changed(PixbufRenderer *pr, gint x, gint y, gint width, gint height)
{
	GList *work;

	if (width < 1 || height < 1) return;

	work = pr->source_tiles;
	while (work)
		{
		SourceTile *st;
		gint rx, ry, rw, rh;

		st = work->data;
		work = work->next;

		if (pr_clip_region(st->x, st->y, pr->source_tile_width, pr->source_tile_height,
				   x, y, width, height,
				   &rx, &ry, &rw, &rh))
			{
			GdkPixbuf *pixbuf;

			pixbuf = gdk_pixbuf_new_subpixbuf(st->pixbuf, rx - st->x, ry - st->y, rw, rh);
			if (pr->func_tile_request &&
			    pr->func_tile_request(pr, rx, ry, rw, rh, pixbuf, pr->func_tile_data))
				{
				pr_tile_invalidate_region(pr, rx * pr->scale, ry * pr->scale,
							      rw * pr->scale, rh * pr->scale);
				}
			g_object_unref(pixbuf);
			}
		}
}

static gboolean pr_source_tile_render(PixbufRenderer *pr, ImageTile *it,
				      gint x, gint y, gint w, gint h,
				      gboolean new_data, gboolean fast)
{
	GtkWidget *box;
	GList *list;
	GList *work;
	gboolean draw = FALSE;

	box = GTK_WIDGET(pr);

	if (pr->zoom == 1.0 || pr->scale == 1.0)
		{
		list = pr_source_tile_compute_region(pr, it->x + x, it->y + y, w, h, TRUE);
		work = list;
		while (work)
			{
			SourceTile *st;
			gint rx, ry, rw, rh;

			st = work->data;
			work = work->next;

			if (pr_clip_region(st->x, st->y, pr->source_tile_width, pr->source_tile_height,
					   it->x + x, it->y + y, w, h,
					   &rx, &ry, &rw, &rh))
				{
				if (st->blank)
					{
					gdk_draw_rectangle(it->pixmap, box->style->black_gc, TRUE,
							   rx - st->x, ry - st->y, rw, rh);
					}
				else /* (pr->zoom == 1.0 || pr->scale == 1.0) */
					{
					gdk_draw_pixbuf(it->pixmap,
#if GTK_CHECK_VERSION(2,20,0)
							box->style->fg_gc[gtk_widget_get_state(box)],
#else
							box->style->fg_gc[GTK_WIDGET_STATE(box)],
#endif
							st->pixbuf,
							rx - st->x, ry - st->y,
							rx - it->x, ry - it->y,
							rw, rh,
							pr->dither_quality, rx, ry);
					}
				}
			}
		}
	else
		{
		gdouble scale_x, scale_y;
		gint sx, sy, sw, sh;

		if (pr->image_width == 0 || pr->image_height == 0) return FALSE;
		scale_x = (gdouble)pr->width / pr->image_width;
		scale_y = (gdouble)pr->height / pr->image_height;

		sx = (gdouble)(it->x + x) / scale_x;
		sy = (gdouble)(it->y + y) / scale_y;
		sw = (gdouble)w / scale_x;
		sh = (gdouble)h / scale_y;

		if (pr->width < PR_MIN_SCALE_SIZE || pr->height < PR_MIN_SCALE_SIZE) fast = TRUE;

#if 0
		/* draws red over draw region, to check for leaks (regions not filled) */
		pixbuf_set_rect_fill(it->pixbuf, x, y, w, h, 255, 0, 0, 255);
#endif

		list = pr_source_tile_compute_region(pr, sx, sy, sw, sh, TRUE);
		work = list;
		while (work)
			{
			SourceTile *st;
			gint rx, ry, rw, rh;
			gint stx, sty, stw, sth;

			st = work->data;
			work = work->next;

			stx = floor((gdouble)st->x * scale_x);
			sty = floor((gdouble)st->y * scale_y);
			stw = ceil((gdouble)(st->x + pr->source_tile_width) * scale_x) - stx;
			sth = ceil((gdouble)(st->y + pr->source_tile_height) * scale_y) - sty;

			if (pr_clip_region(stx, sty, stw, sth,
					   it->x + x, it->y + y, w, h,
					   &rx, &ry, &rw, &rh))
				{
				if (st->blank)
					{
					gdk_draw_rectangle(it->pixmap, box->style->black_gc, TRUE,
							   rx - st->x, ry - st->y, rw, rh);
					}
				else
					{
					gdouble offset_x;
					gdouble offset_y;

					/* may need to use unfloored stx,sty values here */
					offset_x = (gdouble)(stx - it->x);
					offset_y = (gdouble)(sty - it->y);

					gdk_pixbuf_scale(st->pixbuf, it->pixbuf, rx - it->x, ry - it->y, rw, rh,
						 (gdouble) 0.0 + offset_x,
						 (gdouble) 0.0 + offset_y,
						 scale_x, scale_y,
						 (fast) ? GDK_INTERP_NEAREST : pr->zoom_quality);
					draw = TRUE;
					}
				}
			}
		}

	g_list_free(list);

	return draw;
}

void pixbuf_renderer_set_tiles(PixbufRenderer *pr, gint width, gint height,
			       gint tile_width, gint tile_height, gint cache_size,
			       PixbufRendererTileRequestFunc func_request,
			       PixbufRendererTileDisposeFunc func_dispose,
			       gpointer user_data,
			       gdouble zoom)
{
	g_return_if_fail(IS_PIXBUF_RENDERER(pr));
	g_return_if_fail(tile_width >= 32 && tile_width >= 32);
	g_return_if_fail(width >= 32 && height > 32);
	g_return_if_fail(func_request != NULL);

	if (pr->pixbuf) g_object_unref(pr->pixbuf);
	pr->pixbuf = NULL;

	pr_source_tile_unset(pr);

	if (cache_size < 4) cache_size = 4;

	pr->source_tiles_enabled = TRUE;
	pr->source_tiles_cache_size = cache_size;
	pr->source_tile_width = tile_width;
	pr->source_tile_height = tile_height;

	pr->image_width = width;
	pr->image_height = height;

	pr->func_tile_request = func_request;
	pr->func_tile_dispose = func_dispose;
	pr->func_tile_data = user_data;

	pr_zoom_sync(pr, zoom, PR_ZOOM_FORCE | PR_ZOOM_NEW, 0, 0);
	pr_redraw(pr, TRUE);
}

void pixbuf_renderer_set_tiles_size(PixbufRenderer *pr, gint width, gint height)
{
	g_return_if_fail(IS_PIXBUF_RENDERER(pr));
	g_return_if_fail(width >= 32 && height > 32);

	if (!pr->source_tiles_enabled) return;
	if (pr->image_width == width && pr->image_height == height) return;

	pr->image_width = width;
	pr->image_height = height;

	pr_zoom_sync(pr, pr->zoom, PR_ZOOM_FORCE, 0, 0);
}

gint pixbuf_renderer_get_tiles(PixbufRenderer *pr)
{
	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), FALSE);

	return pr->source_tiles_enabled;
}

static void pr_zoom_adjust_real(PixbufRenderer *pr, gdouble increment,
				PrZoomFlags flags, gint x, gint y)
{
	gdouble zoom = pr->zoom;

	if (increment == 0.0) return;

	if (zoom == 0.0)
		{
		if (pr->scale < 1.0)
			{
			zoom = 0.0 - 1.0 / pr->scale;
			}
		else
			{
			zoom = pr->scale;
			}
		}

	if (increment < 0.0)
		{
		if (zoom >= 1.0 && zoom + increment < 1.0)
			{
			zoom = zoom + increment - 2.0;
			}
		else
			{
			zoom = zoom + increment;
			}
		}
	else
		{
		if (zoom <= -1.0 && zoom + increment > -1.0)
			{
			zoom = zoom + increment + 2.0;
			}
		else
			{
			zoom = zoom + increment;
			}
		}

	pr_zoom_sync(pr, zoom, flags, x, y);
}

/*
 *-------------------------------------------------------------------
 * display tiles
 *-------------------------------------------------------------------
 */

static ImageTile *pr_tile_new(gint x, gint y, gint width, gint height)
{
	ImageTile *it;

	it = g_new0(ImageTile, 1);

	it->x = x;
	it->y = y;
	it->w = width;
	it->h = height;

	it->render_done = TILE_RENDER_NONE;

	return it;
}

static void pr_tile_free(ImageTile *it)
{
	if (!it) return;

	if (it->pixbuf) g_object_unref(it->pixbuf);
	if (it->pixmap) g_object_unref(it->pixmap);

	g_free(it);
}

static void pr_tile_free_all(PixbufRenderer *pr)
{
	GList *work;

	work = pr->tiles;
	while (work)
		{
		ImageTile *it;

		it = work->data;
		work = work->next;

		pr_tile_free(it);
		}

	g_list_free(pr->tiles);
	pr->tiles = NULL;
	pr->tile_cache_size = 0;
}

static ImageTile *pr_tile_add(PixbufRenderer *pr, gint x, gint y)
{
	ImageTile *it;

	it = pr_tile_new(x, y, pr->tile_width, pr->tile_height);

	if (it->x + it->w > pr->width) it->w = pr->width - it->x;
	if (it->y + it->h > pr->height) it->h = pr->height - it->y;

	pr->tiles = g_list_prepend(pr->tiles, it);
	pr->tile_cache_size += it->size;

	return it;
}

static void pr_tile_remove(PixbufRenderer *pr, ImageTile *it)
{
	if (it->qd)
		{
		QueueData *qd = it->qd;

		it->qd = NULL;
		pr->draw_queue = g_list_remove(pr->draw_queue, qd);
		g_free(qd);
		}

	if (it->qd2)
		{
		QueueData *qd = it->qd2;

		it->qd2 = NULL;
		pr->draw_queue_2pass = g_list_remove(pr->draw_queue_2pass, qd);
		g_free(qd);
		}

	pr->tiles = g_list_remove(pr->tiles, it);
	pr->tile_cache_size -= it->size;

	pr_tile_free(it);
}

static void pr_tile_free_space(PixbufRenderer *pr, guint space, ImageTile *it)
{
	GList *work;
	guint tile_max;

	work = g_list_last(pr->tiles);

	if (pr->source_tiles_enabled && pr->scale < 1.0)
		{
		gint tiles;

		tiles = (pr->vis_width / pr->tile_width + 1) * (pr->vis_height / pr->tile_height + 1);
		tile_max = MAX(tiles * pr->tile_width * pr->tile_height * 3,
			       (gint)((gdouble)pr->tile_cache_max * 1048576.0 * pr->scale));
		}
	else
		{
		tile_max = pr->tile_cache_max * 1048576;
		}

	while (work && pr->tile_cache_size + space > tile_max)
		{
		ImageTile *needle;

		needle = work->data;
		work = work->prev;
		if (needle != it &&
		    ((!needle->qd && !needle->qd2) || !pr_tile_is_visible(pr, needle))) pr_tile_remove(pr, needle);
		}
}

static void pr_tile_invalidate_all(PixbufRenderer *pr)
{
	GList *work;

	work = pr->tiles;
	while (work)
		{
		ImageTile *it;

		it = work->data;
		work = work->next;

		it->render_done = TILE_RENDER_NONE;
		it->render_todo = TILE_RENDER_ALL;
		it->blank = FALSE;

		it->w = MIN(pr->tile_width, pr->width - it->x);
		it->h = MIN(pr->tile_height, pr->height - it->y);
		}
}

static void pr_tile_invalidate_region(PixbufRenderer *pr, gint x, gint y, gint w, gint h)
{
	gint x1, x2;
	gint y1, y2;
	GList *work;

	x1 = ROUND_DOWN(x, pr->tile_width);
	x2 = ROUND_UP(x + w, pr->tile_width);

	y1 = ROUND_DOWN(y, pr->tile_height);
	y2 = ROUND_UP(y + h, pr->tile_height);

	work = pr->tiles;
	while (work)
		{
		ImageTile *it;

		it = work->data;
		work = work->next;

		if (it->x < x2 && it->x + it->w > x1 &&
		    it->y < y2 && it->y + it->h > y1)
			{
			it->render_done = TILE_RENDER_NONE;
			it->render_todo = TILE_RENDER_ALL;
			}
		}
}

static ImageTile *pr_tile_get(PixbufRenderer *pr, gint x, gint y, gboolean only_existing)
{
	GList *work;

	work = pr->tiles;
	while (work)
		{
		ImageTile *it;

		it = work->data;
		if (it->x == x && it->y == y)
			{
			pr->tiles = g_list_delete_link(pr->tiles, work);
			pr->tiles = g_list_prepend(pr->tiles, it);
			return it;
			}

		work = work->next;
		}

	if (only_existing) return NULL;

	return pr_tile_add(pr, x, y);
}

static void pr_tile_prepare(PixbufRenderer *pr, ImageTile *it)
{
	if (!it->pixmap)
		{
		GdkPixmap *pixmap;
		guint size;

		pixmap = gdk_pixmap_new(((GtkWidget *)pr)->window, pr->tile_width, pr->tile_height, -1);

		size = pixmap_calc_size(pixmap);
		pr_tile_free_space(pr, size, it);

		it->pixmap = pixmap;
		it->size += size;
		pr->tile_cache_size += size;
		}

	if ((pr->zoom != 1.0 || pr->source_tiles_enabled || (pr->pixbuf && gdk_pixbuf_get_has_alpha(pr->pixbuf)) ||
	     pr->orientation != EXIF_ORIENTATION_TOP_LEFT || pr->func_post_process) && !it->pixbuf)
		{
		GdkPixbuf *pixbuf;
		guint size;
#if 0
/* I don't think that we need a pixbuf with alpha channel here */
		if (pr->pixbuf)
			{
			pixbuf = gdk_pixbuf_new(gdk_pixbuf_get_colorspace(pr->pixbuf),
						gdk_pixbuf_get_has_alpha(pr->pixbuf),
						gdk_pixbuf_get_bits_per_sample(pr->pixbuf),
						pr->tile_width, pr->tile_height);
			}
		else
#endif
			{
			pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, pr->tile_width, pr->tile_height);
			}

		size = gdk_pixbuf_get_rowstride(pixbuf) * pr->tile_height;
		pr_tile_free_space(pr, size, it);

		it->pixbuf = pixbuf;
		it->size += size;
		pr->tile_cache_size += size;
		}
}

/*
 *-------------------------------------------------------------------
 * drawing
 *-------------------------------------------------------------------
 */


static void pr_tile_coords_map_orientation(PixbufRenderer *pr,
				     gdouble tile_x, gdouble tile_y, /* coordinates of the tile */
				     gdouble image_w, gdouble image_h,
				     gdouble tile_w, gdouble tile_h,
				     gdouble *res_x, gdouble *res_y)
{
	*res_x = tile_x;
	*res_y = tile_y;
	switch (pr->orientation)
		{
		case EXIF_ORIENTATION_TOP_LEFT:
			/* normal -- nothing to do */
			break;
		case EXIF_ORIENTATION_TOP_RIGHT:
			/* mirrored */
			*res_x = image_w - tile_x - tile_w;
			break;
		case EXIF_ORIENTATION_BOTTOM_RIGHT:
			/* upside down */
			*res_x = image_w - tile_x - tile_w;
			*res_y = image_h - tile_y - tile_h;
			break;
		case EXIF_ORIENTATION_BOTTOM_LEFT:
			/* flipped */
			*res_y = image_h - tile_y - tile_h;
			break;
		case EXIF_ORIENTATION_LEFT_TOP:
			*res_x = tile_y;
			*res_y = tile_x;
			break;
		case EXIF_ORIENTATION_RIGHT_TOP:
			/* rotated -90 (270) */
			*res_x = tile_y;
			*res_y = image_w - tile_x - tile_w;
			break;
		case EXIF_ORIENTATION_RIGHT_BOTTOM:
			*res_x = image_h - tile_y - tile_h;
			*res_y = image_w - tile_x - tile_w;
			break;
		case EXIF_ORIENTATION_LEFT_BOTTOM:
			/* rotated 90 */
			*res_x = image_h - tile_y - tile_h;
			*res_y = tile_x;
			break;
		default:
			/* The other values are out of range */
			break;
		}
//	log_printf("tile coord y:%f, ih:%d, th:%f ry:%f\n", tile_y, image_h, tile_h, *res_x);
}

static void pr_tile_region_map_orientation(PixbufRenderer *pr,
				     gint area_x, gint area_y, /* coordinates of the area inside tile */
				     gint tile_w, gint tile_h,
				     gint area_w, gint area_h,
				     gint *res_x, gint *res_y,
				     gint *res_w, gint *res_h)
{
	*res_x = area_x;
	*res_y = area_y;
	*res_w = area_w;
	*res_h = area_h;

	switch (pr->orientation)
		{
		case EXIF_ORIENTATION_TOP_LEFT:
			/* normal -- nothing to do */
			break;
		case EXIF_ORIENTATION_TOP_RIGHT:
			/* mirrored */
			*res_x = tile_w - area_x - area_w;
			break;
		case EXIF_ORIENTATION_BOTTOM_RIGHT:
			/* upside down */
			*res_x = tile_w - area_x - area_w;
			*res_y = tile_h - area_y - area_h;
			break;
		case EXIF_ORIENTATION_BOTTOM_LEFT:
			/* flipped */
			*res_y = tile_h - area_y - area_h;
			break;
		case EXIF_ORIENTATION_LEFT_TOP:
			*res_x = area_y;
			*res_y = area_x;
			*res_w = area_h;
			*res_h = area_w;
			break;
		case EXIF_ORIENTATION_RIGHT_TOP:
			/* rotated -90 (270) */
			*res_x = area_y;
			*res_y = tile_w - area_x - area_w;
			*res_w = area_h;
			*res_h = area_w;
			break;
		case EXIF_ORIENTATION_RIGHT_BOTTOM:
			*res_x = tile_h - area_y - area_h;
			*res_y = tile_w - area_x - area_w;
			*res_w = area_h;
			*res_h = area_w;
			break;
		case EXIF_ORIENTATION_LEFT_BOTTOM:
			/* rotated 90 */
			*res_x = tile_h - area_y - area_h;
			*res_y = area_x;
			*res_w = area_h;
			*res_h = area_w;
			break;
		default:
			/* The other values are out of range */
			break;
		}
//	log_printf("inside y:%d, th:%d, ah:%d ry:%d\n", area_y, tile_h, area_h, *res_x);
}

static void pr_coords_map_orientation_reverse(PixbufRenderer *pr,
				     gint area_x, gint area_y,
				     gint tile_w, gint tile_h,
				     gint area_w, gint area_h,
				     gint *res_x, gint *res_y,
				     gint *res_w, gint *res_h)
{
	*res_x = area_x;
	*res_y = area_y;
	*res_w = area_w;
	*res_h = area_h;

	switch (pr->orientation)
		{
		case EXIF_ORIENTATION_TOP_LEFT:
			/* normal -- nothing to do */
			break;
		case EXIF_ORIENTATION_TOP_RIGHT:
			/* mirrored */
			*res_x = tile_w - area_x - area_w;
			break;
		case EXIF_ORIENTATION_BOTTOM_RIGHT:
			/* upside down */
			*res_x = tile_w - area_x - area_w;
			*res_y = tile_h - area_y - area_h;
			break;
		case EXIF_ORIENTATION_BOTTOM_LEFT:
			/* flipped */
			*res_y = tile_h - area_y - area_h;
			break;
		case EXIF_ORIENTATION_LEFT_TOP:
			*res_x = area_y;
			*res_y = area_x;
			*res_w = area_h;
			*res_h = area_w;
			break;
		case EXIF_ORIENTATION_RIGHT_TOP:
			/* rotated -90 (270) */
			*res_x = tile_w - area_y - area_h;
			*res_y = area_x;
			*res_w = area_h;
			*res_h = area_w;
			break;
		case EXIF_ORIENTATION_RIGHT_BOTTOM:
			*res_x = tile_w - area_y - area_h;
			*res_y = tile_h - area_x - area_w;
			*res_w = area_h;
			*res_h = area_w;
			break;
		case EXIF_ORIENTATION_LEFT_BOTTOM:
			/* rotated 90 */
			*res_x = area_y;
			*res_y = tile_h - area_x - area_w;
			*res_w = area_h;
			*res_h = area_w;
			break;
		default:
			/* The other values are out of range */
			break;
		}
}


static GdkPixbuf *pr_get_spare_tile(PixbufRenderer *pr)
{
	if (!pr->spare_tile) pr->spare_tile = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, pr->tile_width, pr->tile_height);
	return pr->spare_tile;
}

#define COLOR_BYTES 3	/* rgb */

static void pr_tile_rotate_90_clockwise(PixbufRenderer *pr, GdkPixbuf **tile, gint x, gint y, gint w, gint h)
{
	GdkPixbuf *src = *tile;
	GdkPixbuf *dest;
	gint srs, drs;
	guchar *s_pix, *d_pix;
	guchar *sp, *dp;
	guchar *ip, *spi, *dpi;
	gint i, j;
	gint tw = pr->tile_width;

	srs = gdk_pixbuf_get_rowstride(src);
	s_pix = gdk_pixbuf_get_pixels(src);
	spi = s_pix + (x * COLOR_BYTES);

	dest = pr_get_spare_tile(pr);
	drs = gdk_pixbuf_get_rowstride(dest);
	d_pix = gdk_pixbuf_get_pixels(dest);
	dpi = d_pix + (tw - 1) * COLOR_BYTES;

	for (i = y; i < y + h; i++)
		{
		sp = spi + (i * srs);
		ip = dpi - (i * COLOR_BYTES);
		for (j = x; j < x + w; j++)
			{
			dp = ip + (j * drs);
			memcpy(dp, sp, COLOR_BYTES);
			sp += COLOR_BYTES;
			}
		}

	pr->spare_tile = src;
	*tile = dest;
}

static void pr_tile_rotate_90_counter_clockwise(PixbufRenderer *pr, GdkPixbuf **tile, gint x, gint y, gint w, gint h)
{
	GdkPixbuf *src = *tile;
	GdkPixbuf *dest;
	gint srs, drs;
	guchar *s_pix, *d_pix;
	guchar *sp, *dp;
	guchar *ip, *spi, *dpi;
	gint i, j;
	gint th = pr->tile_height;

	srs = gdk_pixbuf_get_rowstride(src);
	s_pix = gdk_pixbuf_get_pixels(src);
	spi = s_pix + (x * COLOR_BYTES);

	dest = pr_get_spare_tile(pr);
	drs = gdk_pixbuf_get_rowstride(dest);
	d_pix = gdk_pixbuf_get_pixels(dest);
	dpi = d_pix + (th - 1) * drs;

	for (i = y; i < y + h; i++)
		{
		sp = spi + (i * srs);
		ip = dpi + (i * COLOR_BYTES);
		for (j = x; j < x + w; j++)
			{
			dp = ip - (j * drs);
			memcpy(dp, sp, COLOR_BYTES);
			sp += COLOR_BYTES;
			}
		}

	pr->spare_tile = src;
	*tile = dest;
}

static void pr_tile_mirror_only(PixbufRenderer *pr, GdkPixbuf **tile, gint x, gint y, gint w, gint h)
{
	GdkPixbuf *src = *tile;
	GdkPixbuf *dest;
	gint srs, drs;
	guchar *s_pix, *d_pix;
	guchar *sp, *dp;
	guchar *spi, *dpi;
	gint i, j;

	gint tw = pr->tile_width;

	srs = gdk_pixbuf_get_rowstride(src);
	s_pix = gdk_pixbuf_get_pixels(src);
	spi = s_pix + (x * COLOR_BYTES);

	dest = pr_get_spare_tile(pr);
	drs = gdk_pixbuf_get_rowstride(dest);
	d_pix = gdk_pixbuf_get_pixels(dest);
	dpi =  d_pix + (tw - x - 1) * COLOR_BYTES;

	for (i = y; i < y + h; i++)
		{
		sp = spi + (i * srs);
		dp = dpi + (i * drs);
		for (j = 0; j < w; j++)
			{
			memcpy(dp, sp, COLOR_BYTES);
			sp += COLOR_BYTES;
			dp -= COLOR_BYTES;
			}
		}

	pr->spare_tile = src;
	*tile = dest;
}

static void pr_tile_mirror_and_flip(PixbufRenderer *pr, GdkPixbuf **tile, gint x, gint y, gint w, gint h)
{
	GdkPixbuf *src = *tile;
	GdkPixbuf *dest;
	gint srs, drs;
	guchar *s_pix, *d_pix;
	guchar *sp, *dp;
	guchar *spi, *dpi;
	gint i, j;
	gint tw = pr->tile_width;
	gint th = pr->tile_height;

	srs = gdk_pixbuf_get_rowstride(src);
	s_pix = gdk_pixbuf_get_pixels(src);
	spi = s_pix + (x * COLOR_BYTES);

	dest = pr_get_spare_tile(pr);
	drs = gdk_pixbuf_get_rowstride(dest);
	d_pix = gdk_pixbuf_get_pixels(dest);
	dpi = d_pix + (th - 1) * drs + (tw - 1) * COLOR_BYTES;

	for (i = y; i < y + h; i++)
		{
		sp = s_pix + (i * srs) + (x * COLOR_BYTES);
		dp = dpi - (i * drs) - (x * COLOR_BYTES);
		for (j = 0; j < w; j++)
			{
			memcpy(dp, sp, COLOR_BYTES);
			sp += COLOR_BYTES;
			dp -= COLOR_BYTES;
			}
		}

	pr->spare_tile = src;
	*tile = dest;
}

static void pr_tile_flip_only(PixbufRenderer *pr, GdkPixbuf **tile, gint x, gint y, gint w, gint h)
{
	GdkPixbuf *src = *tile;
	GdkPixbuf *dest;
	gint srs, drs;
	guchar *s_pix, *d_pix;
	guchar *sp, *dp;
	guchar *spi, *dpi;
	gint i;
	gint th = pr->tile_height;

	srs = gdk_pixbuf_get_rowstride(src);
	s_pix = gdk_pixbuf_get_pixels(src);
	spi = s_pix + (x * COLOR_BYTES);

	dest = pr_get_spare_tile(pr);
	drs = gdk_pixbuf_get_rowstride(dest);
	d_pix = gdk_pixbuf_get_pixels(dest);
	dpi = d_pix + (th - 1) * drs + (x * COLOR_BYTES);

	for (i = y; i < y + h; i++)
		{
		sp = spi + (i * srs);
		dp = dpi - (i * drs);
		memcpy(dp, sp, w * COLOR_BYTES);
		}

	pr->spare_tile = src;
	*tile = dest;
}

static void pr_tile_apply_orientation(PixbufRenderer *pr, GdkPixbuf **pixbuf, gint x, gint y, gint w, gint h)
{
	switch (pr->orientation)
		{
		case EXIF_ORIENTATION_TOP_LEFT:
			/* normal -- nothing to do */
			break;
		case EXIF_ORIENTATION_TOP_RIGHT:
			/* mirrored */
			{
				pr_tile_mirror_only(pr, pixbuf, x, y, w, h);
			}
			break;
		case EXIF_ORIENTATION_BOTTOM_RIGHT:
			/* upside down */
			{
				pr_tile_mirror_and_flip(pr, pixbuf, x, y, w, h);
			}
			break;
		case EXIF_ORIENTATION_BOTTOM_LEFT:
			/* flipped */
			{
				pr_tile_flip_only(pr, pixbuf, x, y, w, h);
			}
			break;
		case EXIF_ORIENTATION_LEFT_TOP:
			{
				pr_tile_flip_only(pr, pixbuf, x, y, w, h);
				pr_tile_rotate_90_clockwise(pr, pixbuf, x, pr->tile_height - y - h, w, h);
			}
			break;
		case EXIF_ORIENTATION_RIGHT_TOP:
			/* rotated -90 (270) */
			{
				pr_tile_rotate_90_clockwise(pr, pixbuf, x, y, w, h);
			}
			break;
		case EXIF_ORIENTATION_RIGHT_BOTTOM:
			{
				pr_tile_flip_only(pr, pixbuf, x, y, w, h);
				pr_tile_rotate_90_counter_clockwise(pr, pixbuf, x, pr->tile_height - y - h, w, h);
			}
			break;
		case EXIF_ORIENTATION_LEFT_BOTTOM:
			/* rotated 90 */
			{
				pr_tile_rotate_90_counter_clockwise(pr, pixbuf, x, y, w, h);
			}
			break;
		default:
			/* The other values are out of range */
			break;
		}
}


static void pr_tile_render(PixbufRenderer *pr, ImageTile *it,
			   gint x, gint y, gint w, gint h,
			   gboolean new_data, gboolean fast)
{
	GtkWidget *box;
	gboolean has_alpha;
	gboolean draw = FALSE;

	if (it->render_todo == TILE_RENDER_NONE && it->pixmap && !new_data) return;

	if (it->render_done != TILE_RENDER_ALL)
		{
		x = 0;
		y = 0;
		w = it->w;
		h = it->h;
		if (!fast) it->render_done = TILE_RENDER_ALL;
		}
	else if (it->render_todo != TILE_RENDER_AREA)
		{
		if (!fast) it->render_todo = TILE_RENDER_NONE;
		return;
		}

	if (!fast) it->render_todo = TILE_RENDER_NONE;

	if (new_data) it->blank = FALSE;

	pr_tile_prepare(pr, it);
	has_alpha = (pr->pixbuf && gdk_pixbuf_get_has_alpha(pr->pixbuf));

	box = GTK_WIDGET(pr);

	/* FIXME checker colors for alpha should be configurable,
	 * also should be drawn for blank = TRUE
	 */

	if (it->blank)
		{
		/* no data, do fast rect fill */
		gdk_draw_rectangle(it->pixmap, box->style->black_gc, TRUE,
				   0, 0, it->w, it->h);
		}
	else if (pr->source_tiles_enabled)
		{
		draw = pr_source_tile_render(pr, it, x, y, w, h, new_data, fast);
		}
	else if (pr->zoom == 1.0 || pr->scale == 1.0)
		{

		gdouble src_x, src_y;
		gint pb_x, pb_y;
		gint pb_w, pb_h;
		pr_tile_coords_map_orientation(pr, it->x, it->y,
					    pr->image_width, pr->image_height,
					    pr->tile_width, pr->tile_height,
					    &src_x, &src_y);
		pr_tile_region_map_orientation(pr, x, y,
					    pr->tile_width, pr->tile_height,
					    w, h,
					    &pb_x, &pb_y,
					    &pb_w, &pb_h);

		if (has_alpha)
			{
			gdk_pixbuf_composite_color(pr->pixbuf, it->pixbuf, pb_x, pb_y, pb_w, pb_h,
					 (gdouble) 0.0 - src_x,
					 (gdouble) 0.0 - src_y,
					 1.0, 1.0, GDK_INTERP_NEAREST,
					 255, it->x + pb_x, it->y + pb_y,
					 PR_ALPHA_CHECK_SIZE, PR_ALPHA_CHECK1, PR_ALPHA_CHECK2);
			pr_tile_apply_orientation(pr, &it->pixbuf, pb_x, pb_y, pb_w, pb_h);
			draw = TRUE;
			}
		else
			{


			if (pr->orientation == EXIF_ORIENTATION_TOP_LEFT && !(pr->func_post_process && !(pr->post_process_slow && fast)))
				{
				/* faster, simple, base orientation, no postprocessing */
				gdk_draw_pixbuf(it->pixmap,
#if GTK_CHECK_VERSION(2,20,0)
						box->style->fg_gc[gtk_widget_get_state(box)],
#else
						box->style->fg_gc[GTK_WIDGET_STATE(box)],
#endif
						pr->pixbuf,
						it->x + x, it->y + y,
						x, y,
						w, h,
						pr->dither_quality, it->x + x, it->y + y);
				}
			else
				{
				gdk_pixbuf_copy_area(pr->pixbuf,
						     src_x + pb_x, src_y + pb_y,
						     pb_w, pb_h,
						     it->pixbuf,
						     pb_x, pb_y);
				pr_tile_apply_orientation(pr, &it->pixbuf, pb_x, pb_y, pb_w, pb_h);
				draw = TRUE;
				}
			}
		}
	else
		{
		gdouble scale_x, scale_y;
		gdouble src_x, src_y;
		gint pb_x, pb_y;
		gint pb_w, pb_h;

		if (pr->image_width == 0 || pr->image_height == 0) return;

		scale_x = (gdouble)pr->width / pr->image_width;
		scale_y = (gdouble)pr->height / pr->image_height;

		pr_tile_coords_map_orientation(pr, it->x, it->y,
					    pr->image_width * scale_x, pr->image_height * scale_y,
					    pr->tile_width, pr->tile_height,
					    &src_x, &src_y);
		pr_tile_region_map_orientation(pr, x, y,
					    pr->tile_width, pr->tile_height,
					    w, h,
					    &pb_x, &pb_y,
					    &pb_w, &pb_h);
		switch (pr->orientation)
			{
			gdouble tmp;
			case EXIF_ORIENTATION_LEFT_TOP:
			case EXIF_ORIENTATION_RIGHT_TOP:
			case EXIF_ORIENTATION_RIGHT_BOTTOM:
			case EXIF_ORIENTATION_LEFT_BOTTOM:
				tmp = scale_x;
				scale_x = scale_y;
				scale_y = tmp;
				break;
			default:
				/* nothing to do */
				break;
			}

		/* HACK: The pixbuf scalers get kinda buggy(crash) with extremely
		 * small sizes for anything but GDK_INTERP_NEAREST
		 */
		if (pr->width < PR_MIN_SCALE_SIZE || pr->height < PR_MIN_SCALE_SIZE) fast = TRUE;

		if (!has_alpha)
			{
			gdk_pixbuf_scale(pr->pixbuf, it->pixbuf, pb_x, pb_y, pb_w, pb_h,
					 (gdouble) 0.0 - src_x,
					 (gdouble) 0.0 - src_y,
					 scale_x, scale_y,
					 (fast) ? GDK_INTERP_NEAREST : pr->zoom_quality);
			}
		else
			{
			gdk_pixbuf_composite_color(pr->pixbuf, it->pixbuf, pb_x, pb_y, pb_w, pb_h,
					 (gdouble) 0.0 - src_x,
					 (gdouble) 0.0 - src_y,
					 scale_x, scale_y,
					 (fast) ? GDK_INTERP_NEAREST : pr->zoom_quality,
					 255, it->x + pb_x, it->y + pb_y,
					 PR_ALPHA_CHECK_SIZE, PR_ALPHA_CHECK1, PR_ALPHA_CHECK2);
			}
		pr_tile_apply_orientation(pr, &it->pixbuf, pb_x, pb_y, pb_w, pb_h);
		draw = TRUE;
		}

	if (draw && it->pixbuf && !it->blank)
		{

		if (pr->func_post_process && !(pr->post_process_slow && fast))
			pr->func_post_process(pr, &it->pixbuf, x, y, w, h, pr->post_process_user_data);

		gdk_draw_pixbuf(it->pixmap,
#if GTK_CHECK_VERSION(2,20,0)
				box->style->fg_gc[gtk_widget_get_state(box)],
#else
				box->style->fg_gc[GTK_WIDGET_STATE(box)],
#endif
				it->pixbuf,
				x, y,
				x, y,
				w, h,
				pr->dither_quality, it->x + x, it->y + y);
		}

#if 0
	/* enable this line for debugging the edges of tiles */
	gdk_draw_rectangle(it->pixmap, box->style->white_gc,
			   FALSE, 0, 0, it->w, it->h);
	gdk_draw_rectangle(it->pixmap, box->style->white_gc,
			   FALSE, x, y, w, h);
#endif
}


static void pr_tile_expose(PixbufRenderer *pr, ImageTile *it,
			   gint x, gint y, gint w, gint h,
			   gboolean new_data, gboolean fast)
{
	GtkWidget *box;

	pr_tile_render(pr, it, x, y, w, h, new_data, fast);

	box = GTK_WIDGET(pr);

#if GTK_CHECK_VERSION(2,20,0)
	gdk_draw_drawable(box->window, box->style->fg_gc[gtk_widget_get_state(box)],
#else
	gdk_draw_drawable(box->window, box->style->fg_gc[GTK_WIDGET_STATE(box)],
#endif
			  it->pixmap, x, y,
			  pr->x_offset + (it->x - pr->x_scroll) + x, pr->y_offset + (it->y - pr->y_scroll) + y, w, h);

	if (pr->overlay_list)
		{
		pr_overlay_draw(pr, pr->x_offset + (it->x - pr->x_scroll) + x,
				pr->y_offset + (it->y - pr->y_scroll) + y,
				w, h,
				it);
		}
}


static gboolean pr_tile_is_visible(PixbufRenderer *pr, ImageTile *it)
{
	return (it->x + it->w >= pr->x_scroll && it->x < pr->x_scroll + pr->vis_width &&
		it->y + it->h >= pr->y_scroll && it->y < pr->y_scroll + pr->vis_height);
}

/*
 *-------------------------------------------------------------------
 * draw queue
 *-------------------------------------------------------------------
 */

static gint pr_get_queued_area(GList *work)
{
	gint area = 0;
	
	while (work) 
		{
		QueueData *qd = work->data;
		area += qd->w * qd->h;
		work = work->next;
		}
	return area;
}


static gboolean pr_queue_schedule_next_draw(PixbufRenderer *pr, gboolean force_set)
{
	gfloat percent;
	gint visible_area = pr->vis_width * pr->vis_height;
	
	if (!pr->loading)
		{
		/* 2pass prio */ 
		DEBUG_2("redraw priority: 2pass");
		pr->draw_idle_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, pr_queue_draw_idle_cb, pr, NULL);
		return FALSE;
		}
	
	if (visible_area == 0)
		{
		/* not known yet */
		percent = 100.0;
		}
	else
		{
		percent = 100.0 * pr_get_queued_area(pr->draw_queue) / visible_area;
		}
	
	if (percent > 10.0)
		{
		/* we have enough data for starting intensive redrawing */
		DEBUG_2("redraw priority: high %.2f %%", percent);
		pr->draw_idle_id = g_idle_add_full(GDK_PRIORITY_REDRAW, pr_queue_draw_idle_cb, pr, NULL);
		return FALSE;
		}
	
	if (percent < 1.0 || force_set)
		{
		/* queue is (almost) empty, wait  50 ms*/
		DEBUG_2("redraw priority: wait %.2f %%", percent);
		pr->draw_idle_id = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, 50, pr_queue_draw_idle_cb, pr, NULL);
		return FALSE;
		}
	
	/* keep the same priority as before */
	DEBUG_2("redraw priority: no change %.2f %%", percent);
	return TRUE;
}
		

static gboolean pr_queue_draw_idle_cb(gpointer data)
{
	PixbufRenderer *pr = data;
	QueueData *qd;
	gboolean fast;


	if ((!pr->pixbuf && !pr->source_tiles_enabled) ||
	    (!pr->draw_queue && !pr->draw_queue_2pass) ||
	    !pr->draw_idle_id)
		{
		pr_render_complete_signal(pr);

		pr->draw_idle_id = 0;
		return FALSE;
		}

	if (pr->draw_queue)
		{
		qd = pr->draw_queue->data;
		fast = (pr->zoom_2pass && ((pr->zoom_quality != GDK_INTERP_NEAREST && pr->scale != 1.0) || pr->post_process_slow));
		}
	else
		{
		if (pr->loading)
			{
			/* still loading, wait till done (also drops the higher priority) */

			return pr_queue_schedule_next_draw(pr, FALSE);
			}

		qd = pr->draw_queue_2pass->data;
		fast = FALSE;
		}

#if GTK_CHECK_VERSION(2,20,0)
	if (gtk_widget_get_realized(pr))
#else
	if (GTK_WIDGET_REALIZED(pr))
#endif
		{
		if (pr_tile_is_visible(pr, qd->it))
			{
			pr_tile_expose(pr, qd->it, qd->x, qd->y, qd->w, qd->h, qd->new_data, fast);
			}
		else if (qd->new_data)
			{
			/* if new pixel data, and we already have a pixmap, update the tile */
			qd->it->blank = FALSE;
			if (qd->it->pixmap && qd->it->render_done == TILE_RENDER_ALL)
				{
				pr_tile_render(pr, qd->it, qd->x, qd->y, qd->w, qd->h, qd->new_data, fast);
				}
			}
		}

	if (pr->draw_queue)
		{
		qd->it->qd = NULL;
		pr->draw_queue = g_list_remove(pr->draw_queue, qd);
		if (fast)
			{
			if (qd->it->qd2)
				{
				pr_queue_merge(qd->it->qd2, qd);
				g_free(qd);
				}
			else
				{
				qd->it->qd2 = qd;
				pr->draw_queue_2pass = g_list_append(pr->draw_queue_2pass, qd);
				}
			}
		else
			{
			g_free(qd);
			}
		}
	else
		{
		qd->it->qd2 = NULL;
		pr->draw_queue_2pass = g_list_remove(pr->draw_queue_2pass, qd);
		g_free(qd);
		}

	if (!pr->draw_queue && !pr->draw_queue_2pass)
		{
		pr_render_complete_signal(pr);

		pr->draw_idle_id = 0;
		return FALSE;
		}

		return pr_queue_schedule_next_draw(pr, FALSE);
}

static void pr_queue_list_free(GList *list)
{
	GList *work;

	work = list;
	while (work)
		{
		QueueData *qd;

		qd = work->data;
		work = work->next;

		qd->it->qd = NULL;
		qd->it->qd2 = NULL;
		g_free(qd);
		}

	g_list_free(list);
}

static void pr_queue_clear(PixbufRenderer *pr)
{
	pr_queue_list_free(pr->draw_queue);
	pr->draw_queue = NULL;

	pr_queue_list_free(pr->draw_queue_2pass);
	pr->draw_queue_2pass = NULL;

	if (pr->draw_idle_id)
		{
		g_source_remove(pr->draw_idle_id);
		pr->draw_idle_id = 0;
		}
}

static void pr_queue_merge(QueueData *parent, QueueData *qd)
{
	if (parent->x + parent->w < qd->x + qd->w)
		{
		parent->w += (qd->x + qd->w) - (parent->x + parent->w);
		}
	if (parent->x > qd->x)
		{
		parent->w += parent->x - qd->x;
		parent->x = qd->x;
		}

	if (parent->y + parent->h < qd->y + qd->h)
		{
		parent->h += (qd->y + qd->h) - (parent->y + parent->h);
		}
	if (parent->y > qd->y)
		{
		parent->h += parent->y - qd->y;
		parent->y = qd->y;
		}

	parent->new_data |= qd->new_data;
}

static gboolean pr_clamp_to_visible(PixbufRenderer *pr, gint *x, gint *y, gint *w, gint *h)
{
	gint nx, ny;
	gint nw, nh;
	gint vx, vy;
	gint vw, vh;

	vw = pr->vis_width;
	vh = pr->vis_height;

	vx = pr->x_scroll;
	vy = pr->y_scroll;

	if (*x + *w < vx || *x > vx + vw || *y + *h < vy || *y > vy + vh) return FALSE;

	/* now clamp it */
	nx = CLAMP(*x, vx, vx + vw);
	nw = CLAMP(*w - (nx - *x), 1, vw);

	ny = CLAMP(*y, vy, vy + vh);
	nh = CLAMP(*h - (ny - *y), 1, vh);

	*x = nx;
	*y = ny;
	*w = nw;
	*h = nh;

	return TRUE;
}

static gboolean pr_queue_to_tiles(PixbufRenderer *pr, gint x, gint y, gint w, gint h,
				  gboolean clamp, ImageTileRenderType render,
			      	  gboolean new_data, gboolean only_existing)
{
	gint i, j;
	gint x1, x2;
	gint y1, y2;

	if (clamp && !pr_clamp_to_visible(pr, &x, &y, &w, &h)) return FALSE;

	x1 = ROUND_DOWN(x, pr->tile_width);
	x2 = ROUND_UP(x + w, pr->tile_width);

	y1 = ROUND_DOWN(y, pr->tile_height);
	y2 = ROUND_UP(y + h, pr->tile_height);

	for (j = y1; j <= y2; j += pr->tile_height)
		{
		for (i = x1; i <= x2; i += pr->tile_width)
			{
			ImageTile *it;

			it = pr_tile_get(pr, i, j,
					 (only_existing &&
					  (i + pr->tile_width < pr->x_scroll ||
					   i > pr->x_scroll + pr->vis_width ||
					   j + pr->tile_height < pr->y_scroll ||
					   j > pr->y_scroll + pr->vis_height)));
			if (it)
				{
				QueueData *qd;

				if ((render == TILE_RENDER_ALL && it->render_done != TILE_RENDER_ALL) ||
				    (render == TILE_RENDER_AREA && it->render_todo != TILE_RENDER_ALL))
					{
					it->render_todo = render;
					}

				qd = g_new(QueueData, 1);
				qd->it = it;
				qd->new_data = new_data;

				if (i < x)
					{
					qd->x = x - i;
					}
				else
					{
					qd->x = 0;
					}
				qd->w = x + w - i - qd->x;
				if (qd->x + qd->w > pr->tile_width) qd->w = pr->tile_width - qd->x;

				if (j < y)
					{
					qd->y = y - j;
					}
				else
					{
					qd->y = 0;
					}
				qd->h = y + h - j - qd->y;
				if (qd->y + qd->h > pr->tile_height) qd->h = pr->tile_height - qd->y;

				if (qd->w < 1 || qd->h < 1)
					{
					g_free(qd);
					}
				else if (it->qd)
					{
					pr_queue_merge(it->qd, qd);
					g_free(qd);
					}
				else
					{
					it->qd = qd;
					pr->draw_queue = g_list_append(pr->draw_queue, qd);
					}
				}
			}
		}

	return TRUE;
}

static void pr_queue(PixbufRenderer *pr, gint x, gint y, gint w, gint h,
		     gboolean clamp, ImageTileRenderType render,
		     gboolean new_data, gboolean only_existing)
{
	gint nx, ny;

	nx = CLAMP(x, 0, pr->width - 1);
	ny = CLAMP(y, 0, pr->height - 1);
	w -= (nx - x);
	h -= (ny - y);
	w = CLAMP(w, 0, pr->width - nx);
	h = CLAMP(h, 0, pr->height - ny);
	if (w < 1 || h < 1) return;

	if (pr_queue_to_tiles(pr, nx, ny, w, h, clamp, render, new_data, only_existing) &&
	    ((!pr->draw_queue && !pr->draw_queue_2pass) || !pr->draw_idle_id))
		{
		if (pr->draw_idle_id)
			{
			g_source_remove(pr->draw_idle_id);
			pr->draw_idle_id = 0;
			}
		pr_queue_schedule_next_draw(pr, TRUE);
		}
}

static void pr_redraw(PixbufRenderer *pr, gboolean new_data)
{
	pr_queue_clear(pr);
	pr_queue(pr, 0, 0, pr->width, pr->height, TRUE, TILE_RENDER_ALL, new_data, FALSE);
}

/*
 *-------------------------------------------------------------------
 * signal emission
 *-------------------------------------------------------------------
 */

static void pr_update_signal(PixbufRenderer *pr)
{
#if 0
	log_printf("FIXME: send updated signal\n");
#endif
	DEBUG_1("%s pixbuf renderer updated - started drawing %p", get_exec_time(), pr);
	pr->debug_updated = TRUE;
}

static void pr_zoom_signal(PixbufRenderer *pr)
{
	g_signal_emit(pr, signals[SIGNAL_ZOOM], 0, pr->zoom);
}

static void pr_clicked_signal(PixbufRenderer *pr, GdkEventButton *bevent)
{
	g_signal_emit(pr, signals[SIGNAL_CLICKED], 0, bevent);
}

static void pr_scroll_notify_signal(PixbufRenderer *pr)
{
	g_signal_emit(pr, signals[SIGNAL_SCROLL_NOTIFY], 0);
}

static void pr_render_complete_signal(PixbufRenderer *pr)
{
	if (!pr->complete)
		{
		g_signal_emit(pr, signals[SIGNAL_RENDER_COMPLETE], 0);
		g_object_set(G_OBJECT(pr), "complete", TRUE, NULL);
		}
	if (pr->debug_updated)
		{
		DEBUG_1("%s pixbuf renderer done %p", get_exec_time(), pr);
		pr->debug_updated = FALSE;
		}
}

static void pr_drag_signal(PixbufRenderer *pr, GdkEventButton *bevent)
{
	g_signal_emit(pr, signals[SIGNAL_DRAG], 0, bevent);
}

static void pr_update_pixel_signal(PixbufRenderer *pr)
{
	g_signal_emit(pr, signals[SIGNAL_UPDATE_PIXEL], 0);
}

/*
 *-------------------------------------------------------------------
 * sync and clamp
 *-------------------------------------------------------------------
 */

static void pixbuf_renderer_sync_scroll_center(PixbufRenderer *pr)
{
	gint src_x, src_y;
	if (!pr->width || !pr->height) return;

	/* 
	 * Update norm_center only if the image is bigger than the window.
	 * With this condition the stored center survives also a temporary display
	 * of the "broken image" icon.
	*/

	if (pr->width > pr->window_width)
		{
		src_x = pr->x_scroll + pr->vis_width / 2;
		pr->norm_center_x = (gdouble)src_x / pr->width;
		}
	
	if (pr->height > pr->window_height)
		{
		src_y = pr->y_scroll + pr->vis_height / 2;
		pr->norm_center_y = (gdouble)src_y / pr->height;
		}
}


static gboolean pr_scroll_clamp(PixbufRenderer *pr)
{
	gint old_xs;
	gint old_ys;

	if (pr->zoom == 0.0)
		{
		pr->x_scroll = 0;
		pr->y_scroll = 0;

		return FALSE;
		}

	old_xs = pr->x_scroll;
	old_ys = pr->y_scroll;

	if (pr->x_offset > 0)
		{
		pr->x_scroll = 0;
		}
	else
		{
		pr->x_scroll = CLAMP(pr->x_scroll, 0, pr->width - pr->vis_width);
		}

	if (pr->y_offset > 0)
		{
		pr->y_scroll = 0;
		}
	else
		{
		pr->y_scroll = CLAMP(pr->y_scroll, 0, pr->height - pr->vis_height);
		}

	pixbuf_renderer_sync_scroll_center(pr);

	return (old_xs != pr->x_scroll || old_ys != pr->y_scroll);
}

static gboolean pr_size_clamp(PixbufRenderer *pr)
{
	gint old_vw, old_vh;

	old_vw = pr->vis_width;
	old_vh = pr->vis_height;

	if (pr->width < pr->window_width)
		{
		pr->vis_width = pr->width;
		pr->x_offset = (pr->window_width - pr->width) / 2;
		}
	else
		{
		pr->vis_width = pr->window_width;
		pr->x_offset = 0;
		}

	if (pr->height < pr->window_height)
		{
		pr->vis_height = pr->height;
		pr->y_offset = (pr->window_height - pr->height) / 2;
		}
	else
		{
		pr->vis_height = pr->window_height;
		pr->y_offset = 0;
		}

	pixbuf_renderer_sync_scroll_center(pr);

	return (old_vw != pr->vis_width || old_vh != pr->vis_height);
}

static gboolean pr_zoom_clamp(PixbufRenderer *pr, gdouble zoom,
			      PrZoomFlags flags, gboolean *redrawn)
{
	gint w, h;
	gdouble scale;
	gboolean invalid;
	gboolean force = !!(flags & PR_ZOOM_FORCE);
	gboolean new = !!(flags & PR_ZOOM_NEW);
	gboolean invalidate = !!(flags & PR_ZOOM_INVALIDATE);
	gboolean lazy = !!(flags & PR_ZOOM_LAZY);

	zoom = CLAMP(zoom, pr->zoom_min, pr->zoom_max);

	if (pr->zoom == zoom && !force) return FALSE;

	w = pr->image_width;
	h = pr->image_height;

	if (zoom == 0.0 && !pr->pixbuf)
		{
		scale = 1.0;
		}
	else if (zoom == 0.0)
		{
		gint max_w;
		gint max_h;
		gboolean sizeable;

		sizeable = (new && pr_parent_window_sizable(pr));

		if (sizeable)
			{
			max_w = gdk_screen_width();
			max_h = gdk_screen_height();

			if (pr->window_limit)
				{
				max_w = max_w * pr->window_limit_size / 100;
				max_h = max_h * pr->window_limit_size / 100;
				}
			}
		else
			{
			max_w = pr->window_width;
			max_h = pr->window_height;
			}

		if ((pr->zoom_expand && !sizeable) || w > max_w || h > max_h)
			{
			if ((gdouble)max_w / w > (gdouble)max_h / h)
				{
				scale = (gdouble)max_h / h;
				h = max_h;
				w = w * scale + 0.5;
				if (w > max_w) w = max_w;
				}
			else
				{
				scale = (gdouble)max_w / w;
				w = max_w;
				h = h * scale + 0.5;
				if (h > max_h) h = max_h;
				}

			if (pr->autofit_limit)
				{
				gdouble factor = (gdouble)pr->autofit_limit_size / 100;
				w = w * factor + 0.5;
				h = h * factor + 0.5;
				scale = scale * factor;
				}

			if (w < 1) w = 1;
			if (h < 1) h = 1;
			}
		else
			{
			scale = 1.0;
			}
		}
	else if (zoom > 0.0) /* zoom orig, in */
		{
		scale = zoom;
		w = w * scale;
		h = h * scale;
		}
	else /* zoom out */
		{
		scale = 1.0 / (0.0 - zoom);
		w = w * scale;
		h = h * scale;
		}

	invalid = (pr->width != w || pr->height != h);

	pr->zoom = zoom;
	pr->width = w;
	pr->height = h;
	pr->scale = scale;

	if (invalidate || invalid)
		{
		pr_tile_invalidate_all(pr);
		if (!lazy) pr_redraw(pr, TRUE);
		}
	if (redrawn) *redrawn = (invalidate || invalid);

	pixbuf_renderer_sync_scroll_center(pr);

	return TRUE;
}

static void pr_zoom_sync(PixbufRenderer *pr, gdouble zoom,
			 PrZoomFlags flags, gint px, gint py)
{
	gdouble old_scale;
	gint old_cx, old_cy;
	gboolean clamped;
	gboolean sized;
	gboolean redrawn = FALSE;
	gboolean center_point = !!(flags & PR_ZOOM_CENTER);
	gboolean force = !!(flags & PR_ZOOM_FORCE);
	gboolean new = !!(flags & PR_ZOOM_NEW);
	gboolean lazy = !!(flags & PR_ZOOM_LAZY);
	PrZoomFlags clamp_flags = flags;
	gdouble old_center_x = pr->norm_center_x;
	gdouble old_center_y = pr->norm_center_y;
	
	old_scale = pr->scale;
	if (center_point)
		{
		px = CLAMP(px, 0, pr->width);
		py = CLAMP(py, 0, pr->height);
		old_cx = pr->x_scroll + (px - pr->x_offset);
		old_cy = pr->y_scroll + (py - pr->y_offset);
		}
	else
		{
		px = py = 0;
		old_cx = pr->x_scroll + pr->vis_width / 2;
		old_cy = pr->y_scroll + pr->vis_height / 2;
		}

	if (force) clamp_flags |= PR_ZOOM_INVALIDATE;
	if (lazy) clamp_flags |= PR_ZOOM_LAZY;
	if (!pr_zoom_clamp(pr, zoom, clamp_flags, &redrawn)) return;

	clamped = pr_size_clamp(pr);
	sized = pr_parent_window_resize(pr, pr->width, pr->height);

	if (force && new)
		{
		switch (pr->scroll_reset)
			{
			case PR_SCROLL_RESET_NOCHANGE:
				/* maintain old scroll position */
				pr->x_scroll = ((gdouble)pr->image_width * old_center_x * pr->scale) - pr->vis_width / 2;
				pr->y_scroll = ((gdouble)pr->image_height * old_center_y * pr->scale) - pr->vis_height / 2;
				break;
			case PR_SCROLL_RESET_CENTER:
				/* center new image */
				pr->x_scroll = ((gdouble)pr->image_width / 2.0 * pr->scale) - pr->vis_width / 2;
				pr->y_scroll = ((gdouble)pr->image_height / 2.0 * pr->scale) - pr->vis_height / 2;
				break;
			case PR_SCROLL_RESET_TOPLEFT:
			default:
				/* reset to upper left */
				pr->x_scroll = 0;
				pr->y_scroll = 0;
				break;
			}
		}
	else
		{
		/* user zoom does not force, so keep visible center point */
		if (center_point)
			{
			pr->x_scroll = old_cx / old_scale * pr->scale - (px - pr->x_offset);
			pr->y_scroll = old_cy / old_scale * pr->scale - (py - pr->y_offset);
			}
		else
			{
			pr->x_scroll = old_cx / old_scale * pr->scale - (pr->vis_width / 2);
			pr->y_scroll = old_cy / old_scale * pr->scale - (pr->vis_height / 2);
			}
		}

	pr_scroll_clamp(pr);

	/* If the window was not sized, redraw the image - we know there will be no size/expose signal.
	 * But even if a size is claimed, there is no guarantee that the window manager will allow it,
	 * so redraw the window anyway :/
	 */
	if (sized || clamped) pr_border_clear(pr);
	
	if (lazy)
		{
		pr_queue_clear(pr);
		}
	else
		{
		pr_redraw(pr, redrawn);
		}

	pr_scroll_notify_signal(pr);
	pr_zoom_signal(pr);
	pr_update_signal(pr);
}

static void pr_size_sync(PixbufRenderer *pr, gint new_width, gint new_height)
{
	gboolean zoom_changed = FALSE;

	if (pr->window_width == new_width && pr->window_height == new_height) return;

	pr->window_width = new_width;
	pr->window_height = new_height;

	if (pr->zoom == 0.0)
		{
		gdouble old_scale = pr->scale;
		pr_zoom_clamp(pr, 0.0, PR_ZOOM_FORCE, NULL);
		zoom_changed = (old_scale != pr->scale);
		}

	pr_size_clamp(pr);
	pr_scroll_clamp(pr);

	pr_overlay_update_sizes(pr);

	/* ensure scroller remains visible */
	if (pr->scroller_overlay != -1)
		{
		gboolean update = FALSE;

		if (pr->scroller_x > new_width)
			{
			pr->scroller_x = new_width;
			pr->scroller_xpos = new_width;
			update = TRUE;
			}
		if (pr->scroller_y > new_height)
			{
			pr->scroller_y = new_height;
			pr->scroller_ypos = new_height;
			update = TRUE;
			}

		if (update)
			{
			GdkPixbuf *pixbuf;

			if (pixbuf_renderer_overlay_get(pr, pr->scroller_overlay, &pixbuf, NULL, NULL))
				{
				gint w, h;

				w = gdk_pixbuf_get_width(pixbuf);
				h = gdk_pixbuf_get_height(pixbuf);
				pixbuf_renderer_overlay_set(pr, pr->scroller_overlay, pixbuf,
							    pr->scroller_x - w / 2, pr->scroller_y - h / 2);
				}
			}
		}

	pr_border_clear(pr);

	pr_scroll_notify_signal(pr);
	if (zoom_changed) pr_zoom_signal(pr);
	pr_update_signal(pr);
}

static void pr_size_cb(GtkWidget *widget, GtkAllocation *allocation, gpointer data)
{
	PixbufRenderer *pr = data;

	pr_size_sync(pr, allocation->width, allocation->height);
}

static void pixbuf_renderer_paint(PixbufRenderer *pr, GdkRectangle *area)
{
	gint x, y;

	pr_border_draw(pr, area->x, area->y, area->width, area->height);

	x = MAX(0, (gint)area->x - pr->x_offset + pr->x_scroll);
	y = MAX(0, (gint)area->y - pr->y_offset + pr->y_scroll);

	pr_queue(pr, x, y,
		 MIN((gint)area->width, pr->width - x),
		 MIN((gint)area->height, pr->height - y),
		 FALSE, TILE_RENDER_ALL, FALSE, FALSE);
}

/*
 *-------------------------------------------------------------------
 * scrolling
 *-------------------------------------------------------------------
 */

void pixbuf_renderer_scroll(PixbufRenderer *pr, gint x, gint y)
{
	gint old_x, old_y;
	gint x_off, y_off;
	gint w, h;

	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	if (!pr->pixbuf && !pr->source_tiles_enabled) return;

	old_x = pr->x_scroll;
	old_y = pr->y_scroll;

	pr->x_scroll += x;
	pr->y_scroll += y;

	pr_scroll_clamp(pr);
	
	pixbuf_renderer_sync_scroll_center(pr);
	
	if (pr->x_scroll == old_x && pr->y_scroll == old_y) return;

	pr_scroll_notify_signal(pr);

	x_off = pr->x_scroll - old_x;
	y_off = pr->y_scroll - old_y;

	w = pr->vis_width - abs(x_off);
	h = pr->vis_height - abs(y_off);

	if (w < 1 || h < 1)
		{
		/* scrolled completely to new material */
		pr_queue(pr, 0, 0, pr->width, pr->height, TRUE, TILE_RENDER_ALL, FALSE, FALSE);
		return;
		}
	else
		{
		gint x1, y1;
		gint x2, y2;
		GtkWidget *box;
		GdkGC *gc;

		if (x_off < 0)
			{
			x1 = abs(x_off);
			x2 = 0;
			}
		else
			{
			x1 = 0;
			x2 = abs(x_off);
			}

		if (y_off < 0)
			{
			y1 = abs(y_off);
			y2 = 0;
			}
		else
			{
			y1 = 0;
			y2 = abs(y_off);
			}

		box = GTK_WIDGET(pr);

		gc = gdk_gc_new(box->window);
		gdk_gc_set_exposures(gc, TRUE);
		gdk_draw_drawable(box->window, gc,
				  box->window,
				  x2 + pr->x_offset, y2 + pr->y_offset,
				  x1 + pr->x_offset, y1 + pr->y_offset, w, h);
		g_object_unref(gc);

		if (pr->overlay_list)
			{
			pr_overlay_queue_all(pr);
			}

		w = pr->vis_width - w;
		h = pr->vis_height - h;

		if (w > 0)
			{
			pr_queue(pr,
				 x_off > 0 ? pr->x_scroll + (pr->vis_width - w) : pr->x_scroll, pr->y_scroll,
				 w, pr->vis_height, TRUE, TILE_RENDER_ALL, FALSE, FALSE);
			}
		if (h > 0)
			{
			/* FIXME, to optimize this, remove overlap */
			pr_queue(pr,
				 pr->x_scroll, y_off > 0 ? pr->y_scroll + (pr->vis_height - h) : pr->y_scroll,
				 pr->vis_width, h, TRUE, TILE_RENDER_ALL, FALSE, FALSE);
			}

		/* process exposures here, "expose_event" seems to miss a few with obstructed windows */
#if ! GTK_CHECK_VERSION(2,18,0)
		{
		GdkEvent *event;

		while ((event = gdk_event_get_graphics_expose(box->window)) != NULL)
			{
			pixbuf_renderer_paint(pr, &event->expose.area);

			if (event->expose.count == 0)
				{
				gdk_event_free(event);
				break;
				}
			gdk_event_free(event);
			}
		}
#endif
		}
}

void pixbuf_renderer_scroll_to_point(PixbufRenderer *pr, gint x, gint y,
				     gdouble x_align, gdouble y_align)
{
	gint px, py;
	gint ax, ay;

	x_align = CLAMP(x_align, 0.0, 1.0);
	y_align = CLAMP(y_align, 0.0, 1.0);

	ax = (gdouble)pr->vis_width * x_align;
	ay = (gdouble)pr->vis_height * y_align;

	px = (gdouble)x * pr->scale - (pr->x_scroll + ax);
	py = (gdouble)y * pr->scale - (pr->y_scroll + ay);

	pixbuf_renderer_scroll(pr, px, py);
}

/* get or set coordinates of viewport center in the image, in range 0.0 - 1.0 */

void pixbuf_renderer_get_scroll_center(PixbufRenderer *pr, gdouble *x, gdouble *y)
{
	*x = pr->norm_center_x;
	*y = pr->norm_center_y;
}

void pixbuf_renderer_set_scroll_center(PixbufRenderer *pr, gdouble x, gdouble y)
{
	gdouble dst_x, dst_y;

	dst_x = x * pr->width  - pr->vis_width  / 2 - pr->x_scroll + CLAMP(pr->subpixel_x_scroll, -1.0, 1.0);
	dst_y = y * pr->height - pr->vis_height / 2 - pr->y_scroll + CLAMP(pr->subpixel_y_scroll, -1.0, 1.0);

	pr->subpixel_x_scroll = dst_x - (gint)dst_x;
	pr->subpixel_y_scroll = dst_y - (gint)dst_y;

	pixbuf_renderer_scroll(pr, (gint)dst_x, (gint)dst_y);
}

/*
 *-------------------------------------------------------------------
 * mouse
 *-------------------------------------------------------------------
 */

static gboolean pr_mouse_motion_cb(GtkWidget *widget, GdkEventButton *bevent, gpointer data)
{
	PixbufRenderer *pr;
	gint accel;

	/* This is a hack, but work far the best, at least for single pointer systems.
	 * See http://bugzilla.gnome.org/show_bug.cgi?id=587714 for more. */
	gint x, y;
	gdk_window_get_pointer (bevent->window, &x, &y, NULL);
	bevent->x = x;
	bevent->y = y;

	pr = PIXBUF_RENDERER(widget);

	if (pr->scroller_id)
		{
		pr->scroller_xpos = bevent->x;
		pr->scroller_ypos = bevent->y;
		}
	
	pr->x_mouse = bevent->x;
	pr->y_mouse = bevent->y;
	pr_update_pixel_signal(pr);
	
	if (!pr->in_drag || !gdk_pointer_is_grabbed()) return FALSE;

	if (pr->drag_moved < PR_DRAG_SCROLL_THRESHHOLD)
		{
		pr->drag_moved++;
		}
	else
		{
		widget_set_cursor(widget, GDK_FLEUR);
		}

	if (bevent->state & GDK_CONTROL_MASK)
		{
		accel = PR_PAN_SHIFT_MULTIPLIER;
		}
	else
		{
		accel = 1;
		}

	/* do the scroll */
	pixbuf_renderer_scroll(pr, (pr->drag_last_x - bevent->x) * accel,
			       (pr->drag_last_y - bevent->y) * accel);

	pr_drag_signal(pr, bevent);

	pr->drag_last_x = bevent->x;
	pr->drag_last_y = bevent->y;

	/* This is recommended by the GTK+ documentation, but does not work properly.
	 * Use deprecated way until GTK+ gets a solution for correct motion hint handling:
	 * http://bugzilla.gnome.org/show_bug.cgi?id=587714
	 */
	/* gdk_event_request_motions (bevent); */
	return FALSE;
}

static gboolean pr_leave_notify_cb(GtkWidget *widget, GdkEventCrossing *cevent, gpointer data)
{
	PixbufRenderer *pr;

	pr = PIXBUF_RENDERER(widget);
	pr->x_mouse = -1;
	pr->y_mouse = -1;

	pr_update_pixel_signal(pr);
	return FALSE;
}

static gboolean pr_mouse_press_cb(GtkWidget *widget, GdkEventButton *bevent, gpointer data)
{
	PixbufRenderer *pr;
	GtkWidget *parent;

	pr = PIXBUF_RENDERER(widget);

	if (pr->scroller_id) return TRUE;

	switch (bevent->button)
		{
		case MOUSE_BUTTON_LEFT:
			pr->in_drag = TRUE;
			pr->drag_last_x = bevent->x;
			pr->drag_last_y = bevent->y;
			pr->drag_moved = 0;
			gdk_pointer_grab(widget->window, FALSE,
					 GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_RELEASE_MASK,
					 NULL, NULL, bevent->time);
			gtk_grab_add(widget);
			break;
		case MOUSE_BUTTON_MIDDLE:
			pr->drag_moved = 0;
			break;
		case MOUSE_BUTTON_RIGHT:
			pr_clicked_signal(pr, bevent);
			break;
		default:
			break;
		}

	parent = gtk_widget_get_parent(widget);
#if GTK_CHECK_VERSION(2,20,0)
	if (widget && gtk_widget_get_can_focus(parent))
#else
	if (widget && GTK_WIDGET_CAN_FOCUS(parent))
#endif
		{
		gtk_widget_grab_focus(parent);
		}

	return FALSE;
}

static gboolean pr_mouse_release_cb(GtkWidget *widget, GdkEventButton *bevent, gpointer data)
{
	PixbufRenderer *pr;

	pr = PIXBUF_RENDERER(widget);

	if (pr->scroller_id)
		{
		pr_scroller_stop(pr);
		return TRUE;
		}

#if GTK_CHECK_VERSION(2,20,0)
	if (gdk_pointer_is_grabbed() && gtk_widget_has_grab(pr))
#else
	if (gdk_pointer_is_grabbed() && GTK_WIDGET_HAS_GRAB(pr))
#endif
		{
		gtk_grab_remove(widget);
		gdk_pointer_ungrab(bevent->time);
		widget_set_cursor(widget, -1);
		}

	if (pr->drag_moved < PR_DRAG_SCROLL_THRESHHOLD)
		{
		if (bevent->button == MOUSE_BUTTON_LEFT && (bevent->state & GDK_CONTROL_MASK))
			{
			pr_scroller_start(pr, bevent->x, bevent->y);
			}
		else if (bevent->button == MOUSE_BUTTON_LEFT || bevent->button == MOUSE_BUTTON_MIDDLE)
			{
			pr_clicked_signal(pr, bevent);
			}
		}

	pr->in_drag = FALSE;

	return FALSE;
}

static gboolean pr_mouse_leave_cb(GtkWidget *widget, GdkEventCrossing *event, gpointer data)
{
	PixbufRenderer *pr;

	pr = PIXBUF_RENDERER(widget);

	if (pr->scroller_id)
		{
		pr->scroller_xpos = pr->scroller_x;
		pr->scroller_ypos = pr->scroller_y;
		pr->scroller_xinc = 0;
		pr->scroller_yinc = 0;
		}

	return FALSE;
}

static void pr_mouse_drag_cb(GtkWidget *widget, GdkDragContext *context, gpointer data)
{
	PixbufRenderer *pr;

	pr = PIXBUF_RENDERER(widget);

	pr->drag_moved = PR_DRAG_SCROLL_THRESHHOLD;
}

static void pr_signals_connect(PixbufRenderer *pr)
{
	g_signal_connect(G_OBJECT(pr), "motion_notify_event",
			 G_CALLBACK(pr_mouse_motion_cb), pr);
	g_signal_connect(G_OBJECT(pr), "button_press_event",
			 G_CALLBACK(pr_mouse_press_cb), pr);
	g_signal_connect(G_OBJECT(pr), "button_release_event",
			 G_CALLBACK(pr_mouse_release_cb), pr);
	g_signal_connect(G_OBJECT(pr), "leave_notify_event",
			 G_CALLBACK(pr_mouse_leave_cb), pr);
	g_signal_connect(G_OBJECT(pr), "hierarchy-changed",
			 G_CALLBACK(pr_hierarchy_changed_cb), pr);
	g_signal_connect(G_OBJECT(pr), "leave_notify_event",
			 G_CALLBACK(pr_leave_notify_cb), pr);

	gtk_widget_set_events(GTK_WIDGET(pr), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK |
					      GDK_BUTTON_RELEASE_MASK | GDK_BUTTON_PRESS_MASK |
					      GDK_LEAVE_NOTIFY_MASK);

	g_signal_connect(G_OBJECT(pr), "drag_begin",
			 G_CALLBACK(pr_mouse_drag_cb), pr);

}

/*
 *-------------------------------------------------------------------
 * public
 *-------------------------------------------------------------------
 */
static void pr_pixbuf_size_sync(PixbufRenderer *pr)
{
	if (!pr->pixbuf) return;
	switch (pr->orientation)
		{
		case EXIF_ORIENTATION_LEFT_TOP:
		case EXIF_ORIENTATION_RIGHT_TOP:
		case EXIF_ORIENTATION_RIGHT_BOTTOM:
		case EXIF_ORIENTATION_LEFT_BOTTOM:
			pr->image_width = gdk_pixbuf_get_height(pr->pixbuf);
			pr->image_height = gdk_pixbuf_get_width(pr->pixbuf);
			break;
		default:
			pr->image_width = gdk_pixbuf_get_width(pr->pixbuf);
			pr->image_height = gdk_pixbuf_get_height(pr->pixbuf);
		}
}

static void pr_set_pixbuf(PixbufRenderer *pr, GdkPixbuf *pixbuf, gdouble zoom, PrZoomFlags flags)
{
	if (pixbuf) g_object_ref(pixbuf);
	if (pr->pixbuf) g_object_unref(pr->pixbuf);
	pr->pixbuf = pixbuf;

	if (!pr->pixbuf)
		{
		GtkWidget *box;

		/* no pixbuf so just clear the window */
		pr->image_width = 0;
		pr->image_height = 0;
		pr->scale = 1.0;
		pr->zoom = zoom; /* don't throw away the zoom value, it is set by pixbuf_renderer_move, among others,
				    and used for pixbuf_renderer_zoom_get */

		box = GTK_WIDGET(pr);

#if GTK_CHECK_VERSION(2,20,0)
		if (gtk_widget_get_realized(box))
#else
		if (GTK_WIDGET_REALIZED(box))
#endif
			{
			gdk_window_clear(box->window);
			pr_overlay_draw(pr, 0, 0, pr->window_width, pr->window_height, NULL);
			}

		pr_update_signal(pr);

		return;
		}

	pr_pixbuf_size_sync(pr);
	pr_zoom_sync(pr, zoom, flags | PR_ZOOM_FORCE | PR_ZOOM_NEW, 0, 0);
}

void pixbuf_renderer_set_pixbuf(PixbufRenderer *pr, GdkPixbuf *pixbuf, gdouble zoom)
{
	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	pr_source_tile_unset(pr);

	pr_set_pixbuf(pr, pixbuf, zoom, 0);

	pr_update_signal(pr);
}

void pixbuf_renderer_set_pixbuf_lazy(PixbufRenderer *pr, GdkPixbuf *pixbuf, gdouble zoom, gint orientation)
{
	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	pr_source_tile_unset(pr);

	pr->orientation = orientation;
	pr_set_pixbuf(pr, pixbuf, zoom, PR_ZOOM_LAZY);

	pr_update_signal(pr);
}

GdkPixbuf *pixbuf_renderer_get_pixbuf(PixbufRenderer *pr)
{
	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), NULL);

	return pr->pixbuf;
}

void pixbuf_renderer_set_orientation(PixbufRenderer *pr, gint orientation)
{
	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	pr->orientation = orientation;

	pr_pixbuf_size_sync(pr);
	pr_zoom_sync(pr, pr->zoom, PR_ZOOM_FORCE, 0, 0);
}

gint pixbuf_renderer_get_orientation(PixbufRenderer *pr)
{
	if (!pr) return 1;
	return pr->orientation;
}

void pixbuf_renderer_set_post_process_func(PixbufRenderer *pr, PixbufRendererPostProcessFunc func, gpointer user_data, gboolean slow)
{
	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	pr->func_post_process = func;
	pr->post_process_user_data = user_data;
	pr->post_process_slow = func && slow;

}


void pixbuf_renderer_move(PixbufRenderer *pr, PixbufRenderer *source)
{
	GObject *object;
	PixbufRendererScrollResetType scroll_reset;

	g_return_if_fail(IS_PIXBUF_RENDERER(pr));
	g_return_if_fail(IS_PIXBUF_RENDERER(source));

	if (pr == source) return;

	object = G_OBJECT(pr);

	g_object_set(object, "zoom_min", source->zoom_min, NULL);
	g_object_set(object, "zoom_max", source->zoom_max, NULL);
	g_object_set(object, "loading", source->loading, NULL);

	pr->complete = source->complete;

	pr->x_scroll = source->x_scroll;
	pr->y_scroll = source->y_scroll;
	pr->x_mouse  = source->x_mouse;
	pr->y_mouse  = source->y_mouse;

	scroll_reset = pr->scroll_reset;
	pr->scroll_reset = PR_SCROLL_RESET_NOCHANGE;

	pr->func_post_process = source->func_post_process;
	pr->post_process_user_data = source->post_process_user_data;
	pr->post_process_slow = source->post_process_slow;
	pr->orientation = source->orientation;

	if (source->source_tiles_enabled)
		{
		pr_source_tile_unset(pr);

		pr->source_tiles_enabled = source->source_tiles_enabled;
		pr->source_tiles_cache_size = source->source_tiles_cache_size;
		pr->source_tile_width = source->source_tile_width;
		pr->source_tile_height = source->source_tile_height;
		pr->image_width = source->image_width;
		pr->image_height = source->image_height;

		pr->func_tile_request = source->func_tile_request;
		pr->func_tile_dispose = source->func_tile_dispose;
		pr->func_tile_data = source->func_tile_data;

		pr->source_tiles = source->source_tiles;
		source->source_tiles = NULL;

		pr_zoom_sync(pr, source->zoom, PR_ZOOM_FORCE | PR_ZOOM_NEW, 0, 0);
		pr_redraw(pr, TRUE);
		}
	else
		{
		pixbuf_renderer_set_pixbuf(pr, source->pixbuf, source->zoom);
		}

	pr->scroll_reset = scroll_reset;

	pixbuf_renderer_set_pixbuf(source, NULL, source->zoom);
	pr_queue_clear(source);
	pr_tile_free_all(source);
}

void pixbuf_renderer_area_changed(PixbufRenderer *pr, gint src_x, gint src_y, gint src_w, gint src_h)
{
	gint x, y, width, height,  x1, y1, x2, y2;

	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	pr_coords_map_orientation_reverse(pr,
				     src_x, src_y,
				     pr->image_width, pr->image_height,
				     src_w, src_h,
				     &x, &y,
				     &width, &height);

	if (pr->source_tiles_enabled)
		{
		pr_source_tile_changed(pr, x, y, width, height);
		}

	if (pr->scale != 1.0 && pr->zoom_quality != GDK_INTERP_NEAREST)
		{
		/* increase region when using a zoom quality that may access surrounding pixels */
		y -= 1;
		height += 2;
		}

	x1 = (gint)floor((gdouble)x * pr->scale);
	y1 = (gint)floor((gdouble)y * pr->scale);
	x2 = (gint)ceil((gdouble)(x + width) * pr->scale);
	y2 = (gint)ceil((gdouble)(y + height) * pr->scale);

	pr_queue(pr, x1, y1, x2 - x1, y2 - y1, FALSE, TILE_RENDER_AREA, TRUE, TRUE);
}

void pixbuf_renderer_zoom_adjust(PixbufRenderer *pr, gdouble increment)
{
	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	pr_zoom_adjust_real(pr, increment, PR_ZOOM_NONE, 0, 0);
}

void pixbuf_renderer_zoom_adjust_at_point(PixbufRenderer *pr, gdouble increment, gint x, gint y)
{
	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	pr_zoom_adjust_real(pr, increment, PR_ZOOM_CENTER, x, y);
}

void pixbuf_renderer_zoom_set(PixbufRenderer *pr, gdouble zoom)
{
	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	pr_zoom_sync(pr, zoom, PR_ZOOM_NONE, 0, 0);
}

gdouble pixbuf_renderer_zoom_get(PixbufRenderer *pr)
{
	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), 1.0);

	return pr->zoom;
}

gdouble pixbuf_renderer_zoom_get_scale(PixbufRenderer *pr)
{
	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), 1.0);

	return pr->scale;
}

void pixbuf_renderer_zoom_set_limits(PixbufRenderer *pr, gdouble min, gdouble max)
{
	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	if (min > 1.0 || max < 1.0) return;
	if (min < 1.0 && min > -1.0) return;
	if (min < -200.0 || max > 200.0) return;

	if (pr->zoom_min != min)
		{
		pr->zoom_min = min;
		g_object_notify(G_OBJECT(pr), "zoom_min");
		}
	if (pr->zoom_max != max)
		{
		pr->zoom_max = max;
		g_object_notify(G_OBJECT(pr), "zoom_max");
		}
}

gboolean pixbuf_renderer_get_pixel_colors(PixbufRenderer *pr, gint x_pixel, gint y_pixel, 
                                          gint *r_mouse, gint *g_mouse, gint *b_mouse)
{
	GdkPixbuf *pb = pr->pixbuf;
	gint p_alpha, prs;
	guchar *p_pix, *pp;
	gint map_x, map_y, map_w, map_h;
	
	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), FALSE);
	g_return_val_if_fail(r_mouse != NULL && g_mouse != NULL && b_mouse != NULL, FALSE);

	if (!pr->pixbuf && !pr->source_tiles_enabled)
		{
		*r_mouse = -1;
		*g_mouse = -1;
		*b_mouse = -1;
		return FALSE;
		}
	
	if (!pb) return FALSE;

	pr_tile_region_map_orientation(pr,
					x_pixel, y_pixel,
					pr->image_width, pr->image_height,
					1, 1, /*single pixel */
					&map_x, &map_y,
					&map_w, &map_h);

	if (map_x < 0 || map_x > gdk_pixbuf_get_width(pr->pixbuf) - 1) return  FALSE;
	if (map_y < 0 || map_y > gdk_pixbuf_get_height(pr->pixbuf) - 1) return  FALSE;
	
	p_alpha = gdk_pixbuf_get_has_alpha(pb);
	prs = gdk_pixbuf_get_rowstride(pb);
	p_pix = gdk_pixbuf_get_pixels(pb);

	pp = p_pix + map_y * prs + (map_x * (p_alpha ? 4 : 3));
	*r_mouse = *pp;
	pp++;
	*g_mouse = *pp;
	pp++;
	*b_mouse = *pp;
	
	return TRUE;
}

gboolean pixbuf_renderer_get_mouse_position(PixbufRenderer *pr, gint *x_pixel_return, gint *y_pixel_return)
{
	gint x_pixel, y_pixel, x_pixel_clamped, y_pixel_clamped;
	     
	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), FALSE);
	g_return_val_if_fail(x_pixel_return != NULL && y_pixel_return != NULL, FALSE);

	if (!pr->pixbuf && !pr->source_tiles_enabled)
		{
		*x_pixel_return = -1;
		*y_pixel_return = -1;
		return FALSE;
		}
	
	x_pixel = floor((gdouble)(pr->x_mouse - pr->x_offset + pr->x_scroll) / pr->scale);
	y_pixel = floor((gdouble)(pr->y_mouse - pr->y_offset + pr->y_scroll) / pr->scale);
	x_pixel_clamped = CLAMP(x_pixel, 0, pr->image_width - 1);
	y_pixel_clamped = CLAMP(y_pixel, 0, pr->image_height - 1);
	
	if(x_pixel != x_pixel_clamped || y_pixel != y_pixel_clamped)
		{
		/* mouse is not on pr */
		x_pixel = y_pixel = -1;
		}

	*x_pixel_return = x_pixel;
	*y_pixel_return = y_pixel;
	
	return TRUE;
}

gboolean pixbuf_renderer_get_image_size(PixbufRenderer *pr, gint *width, gint *height)
{
	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), FALSE);
	g_return_val_if_fail(width != NULL && height != NULL, FALSE);

	if (!pr->pixbuf && !pr->source_tiles_enabled)
		{
		*width = 0;
		*height = 0;
		return FALSE;
		}

	*width = pr->image_width;
	*height = pr->image_height;
	return TRUE;
}

gboolean pixbuf_renderer_get_scaled_size(PixbufRenderer *pr, gint *width, gint *height)
{
	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), FALSE);
	g_return_val_if_fail(width != NULL && height != NULL, FALSE);

	if (!pr->pixbuf && !pr->source_tiles_enabled)
		{
		*width = 0;
		*height = 0;
		return FALSE;
		}

	*width = pr->width;
	*height = pr->height;
	return TRUE;
}

gboolean pixbuf_renderer_get_visible_rect(PixbufRenderer *pr, GdkRectangle *rect)
{
	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), FALSE);
	g_return_val_if_fail(rect != NULL, FALSE);

	if ((!pr->pixbuf && !pr->source_tiles_enabled) ||
	    !pr->scale)
		{
		rect->x = 0;
		rect->y = 0;
		rect->width = 0;
		rect->height = 0;
		return FALSE;
		}

	rect->x = (gint)((gdouble)pr->x_scroll / pr->scale);
	rect->y = (gint)((gdouble)pr->y_scroll / pr->scale);
	rect->width = (gint)((gdouble)pr->vis_width / pr->scale);
	rect->height = (gint)((gdouble)pr->vis_height / pr->scale);
	return TRUE;
}

gboolean pixbuf_renderer_get_virtual_rect(PixbufRenderer *pr, GdkRectangle *rect)
{
	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), FALSE);
	g_return_val_if_fail(rect != NULL, FALSE);

	if ((!pr->pixbuf && !pr->source_tiles_enabled))
		{
		rect->x = 0;
		rect->y = 0;
		rect->width = 0;
		rect->height = 0;
		return FALSE;
		}

	rect->x = pr->x_scroll;
	rect->y = pr->y_scroll;
	rect->width = pr->vis_width;
	rect->height = pr->vis_height;
	return TRUE;
}
/* vim: set shiftwidth=8 softtabstop=0 cindent cinoptions={1s: */
