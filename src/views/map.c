/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.
    copyright (c) 2012 tobias ellinghaus.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "control/control.h"
#include "control/conf.h"
#include "common/debug.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "views/view.h"
#include "libs/lib.h"
#include "gui/drag_and_drop.h"

#include "osm-gps-map.h"

DT_MODULE(1)


typedef struct dt_map_t
{
  GtkWidget *center;
  OsmGpsMap *map;
  OsmGpsMapLayer *osd;
  GSList *images;
  gint selected_image;
  gboolean start_drag;
  struct
  {
    sqlite3_stmt *main_query;
  } statements;
} dt_map_t;

typedef struct dt_map_image_t
{
  gint imgid;
  OsmGpsMapImage *image;
  gint width, height;
} dt_map_image_t;

/* proxy function to center map view on location at a zoom level */
static void _view_map_center_on_location(const dt_view_t *view, gdouble lon, gdouble lat, gdouble zoom);
/* proxy function to show or hide the osd */
static void _view_map_show_osd(const dt_view_t *view, gboolean enabled);
/* proxy function to set the map source */
static void _view_map_set_map_source(const dt_view_t *view, OsmGpsMapSource_t map_source);
/* callback when an image is selected in filmstrip, centers map */
static void _view_map_filmstrip_activate_callback(gpointer instance, gpointer user_data);
/* callback when an image is dropped from filmstrip */
static void drag_and_drop_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection_data,
                                   guint target_type, guint time, gpointer data);
/* callback when the user drags images FROM the map */
static void _view_map_dnd_get_callback(GtkWidget *widget, GdkDragContext *context,
                                       GtkSelectionData *selection_data, guint target_type, guint time, dt_view_t *self);
/* callback that readds the images to the map */
static void _view_map_changed_callback(OsmGpsMap *map, dt_view_t *self);
/* callback that handles double clicks on the map */
static gboolean _view_map_button_press_callback(GtkWidget *w, GdkEventButton *e, dt_view_t *self);
/* callback when the mouse is moved */
static gboolean _view_map_motion_notify_callback(GtkWidget *w, GdkEventMotion *e, dt_view_t *self);
static gboolean _view_map_dnd_failed_callback(GtkWidget *widget, GdkDragContext *drag_context, GtkDragResult result, dt_view_t *self);

const char *name(dt_view_t *self)
{
  return _("map");
}

uint32_t view(dt_view_t *self)
{
  return DT_VIEW_MAP;
}

void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_map_t));
  memset(self->data,0,sizeof(dt_map_t));

  dt_map_t *lib = (dt_map_t *)self->data;

  if(darktable.gui)
  {
    OsmGpsMapSource_t map_source = OSM_GPS_MAP_SOURCE_OPENSTREETMAP;
    const gchar *old_map_source = dt_conf_get_string("plugins/map/map_source");
    if(old_map_source && old_map_source[0] != '\0')
    {
      // find the number of the stored map_source
      for(int i=0; i<=OSM_GPS_MAP_SOURCE_LAST; i++)
      {
        const gchar *new_map_source = osm_gps_map_source_get_friendly_name(i);
        if(!g_strcmp0(old_map_source, new_map_source))
        {
          if(osm_gps_map_source_is_valid(i))
            map_source = i;
          break;
        }
      }
    }
    else // open street map should be a nice default ...
      dt_conf_set_string("plugins/map/map_source", osm_gps_map_source_get_friendly_name(OSM_GPS_MAP_SOURCE_OPENSTREETMAP));

    lib->map = g_object_new (OSM_TYPE_GPS_MAP,
                            "map-source", map_source,
                            "proxy-uri",g_getenv("http_proxy"),
                            NULL);

    GtkWidget *parent = gtk_widget_get_parent(dt_ui_center(darktable.gui->ui));
    gtk_box_pack_start(GTK_BOX(parent), GTK_WIDGET(lib->map) ,TRUE, TRUE, 0);

    lib->osd = g_object_new (OSM_TYPE_GPS_MAP_OSD,
                                          "show-scale",TRUE, "show-coordinates",TRUE, "show-dpad",TRUE, "show-zoom",TRUE, NULL);

    if(dt_conf_get_bool("plugins/map/show_map_osd"))
    {
      osm_gps_map_layer_add(OSM_GPS_MAP(lib->map), lib->osd);
    }

    /* allow drag&drop of images from filmstrip */
    gtk_drag_dest_set(GTK_WIDGET(lib->map), GTK_DEST_DEFAULT_ALL, target_list_internal, n_targets_internal, GDK_ACTION_COPY);
    g_signal_connect(GTK_WIDGET(lib->map), "drag-data-received", G_CALLBACK(drag_and_drop_received), self);
    g_signal_connect(GTK_WIDGET(lib->map), "changed", G_CALLBACK(_view_map_changed_callback), self);
    g_signal_connect_after(G_OBJECT(lib->map), "button-press-event", G_CALLBACK(_view_map_button_press_callback), self);
    g_signal_connect (G_OBJECT(lib->map), "motion-notify-event", G_CALLBACK(_view_map_motion_notify_callback), self);

    /* allow drag&drop of images from the map, too */
    g_signal_connect(GTK_WIDGET(lib->map), "drag-data-get", G_CALLBACK(_view_map_dnd_get_callback), self);
    g_signal_connect(GTK_WIDGET(lib->map), "drag-failed", G_CALLBACK(_view_map_dnd_failed_callback), self);
  }

  /* build the query string */
  int max_images_drawn = dt_conf_get_int("plugins/map/max_images_drawn");
  if(max_images_drawn == 0)
    max_images_drawn = 100;
  char *geo_query = g_strdup_printf("select * from (select id from images where \
                              longitude >= ?1 and longitude <= ?2 and latitude <= ?3 and latitude >= ?4 \
                              and longitude not NULL and latitude not NULL order by abs(latitude - ?5), abs(longitude - ?6) \
                              limit 0, %d) order by id", max_images_drawn);

  /* prepare the main query statement */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), geo_query, -1, &lib->statements.main_query, NULL);

  g_free(geo_query);
}

void cleanup(dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  if(darktable.gui)
    g_object_unref(G_OBJECT(lib->osd));
  free(self->data);
}


void configure(dt_view_t *self, int wd, int ht)
{
  //dt_capture_t *lib=(dt_capture_t*)self->data;
}

#if 0
static void _view_map_post_expose(cairo_t *cri, int32_t width_i, int32_t height_i,
                                  int32_t pointerx, int32_t pointery, gpointer user_data)
{
  const int ts = 64;
  OsmGpsMapPoint bb[2], *l=NULL, *center=NULL;
  int px,py;
  dt_map_t *lib = (dt_map_t *)user_data;

  /* get bounding box coords */
  osm_gps_map_get_bbox(lib->map, &bb[0], &bb[1]);
  float bb_0_lat = 0.0, bb_0_lon = 0.0, bb_1_lat = 0.0, bb_1_lon = 0.0;
  osm_gps_map_point_get_degrees(&bb[0], &bb_0_lat, &bb_0_lon);
  osm_gps_map_point_get_degrees(&bb[1], &bb_1_lat, &bb_1_lon);

  /* make the bounding box a little bigger to the west and south */
  float lat0 = 0.0, lon0 = 0.0, lat1 = 0.0, lon1 = 0.0;
  OsmGpsMapPoint *pt0 = osm_gps_map_point_new_degrees(0.0, 0.0), *pt1 = osm_gps_map_point_new_degrees(0.0, 0.0);
  osm_gps_map_convert_screen_to_geographic(lib->map, 0, 0, pt0);
  osm_gps_map_convert_screen_to_geographic(lib->map, 1.5*ts, 1.5*ts, pt1);
  osm_gps_map_point_get_degrees(pt0, &lat0, &lon0);
  osm_gps_map_point_get_degrees(pt1, &lat1, &lon1);
  osm_gps_map_point_free(pt0);
  osm_gps_map_point_free(pt1);
  double south_border = lat0 - lat1, west_border = lon1 - lon0;

  /* get map view state and store  */
  int zoom = osm_gps_map_get_zoom(lib->map);
  center = osm_gps_map_get_center(lib->map);
  dt_conf_set_float("plugins/map/longitude", center->rlon);
  dt_conf_set_float("plugins/map/latitude", center->rlat);
  dt_conf_set_int("plugins/map/zoom", zoom);
  osm_gps_map_point_free(center);

  /* let's reset and reuse the main_query statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
  DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

  /* bind bounding box coords for the main query */
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 1, bb_0_lon - west_border);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 2, bb_1_lon);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 3, bb_0_lat);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 4, bb_1_lat - south_border);

  /* query collection ids */
  while(sqlite3_step(lib->statements.main_query) == SQLITE_ROW)
  {
    int32_t imgid = sqlite3_column_int(lib->statements.main_query, 0);

    cairo_set_source_rgba(cri, 0, 0, 0, 0.4);

    /* free l if allocated */
    if (l)
      osm_gps_map_point_free(l);

    /* for each image check if within bbox */
    const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
    double longitude = cimg->longitude;
    double latitude  = cimg->latitude;
    dt_image_cache_read_release(darktable.image_cache, cimg);
    if(isnan(latitude) || isnan(longitude))
      continue;
    l = osm_gps_map_point_new_degrees(latitude, longitude);

    /* translate l into screen coords */
    osm_gps_map_convert_geographic_to_screen(lib->map, l, &px, &py);

    /* dependent on scale draw different overlays */
    if (zoom >= 14)
    {
      dt_mipmap_buffer_t buf;
      dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, ts, ts);
      dt_mipmap_cache_read_get(darktable.mipmap_cache, &buf, imgid, mip, 0);

      cairo_surface_t *surface = NULL;
      if(buf.buf)
      {
        float ms = fminf(
                     ts/(float)buf.width,
                     ts/(float)buf.height);

#if 0
        // this doesn't work since osm-gps-map always gives 0/0 as mouse coords :(
        /* find out if the cursor is over the image */
        if(pointerx >= px && pointerx <= (px + buf.width*ms + 4) && pointery <= (py - 8) && pointery >= (py - buf.height*ms - 8 - 4))
        {
          printf("over\n");
          cairo_set_source_rgba(cri, 1, 0, 0, 0.7);
        }
//         else
//           printf("%d/%d, %d/%d\n", px, py, pointerx, pointery);
#endif
        const int32_t stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, buf.width);
        surface = cairo_image_surface_create_for_data (buf.buf, CAIRO_FORMAT_RGB24,
                  buf.width, buf.height, stride);

        cairo_pattern_set_filter(cairo_get_source(cri), CAIRO_FILTER_NEAREST);
        cairo_save(cri);

        /* first of lets draw a pin */
        cairo_move_to(cri, px, py);
        cairo_line_to(cri, px+8, py-8);
        cairo_line_to(cri, px+4, py-8);
        cairo_fill(cri);

        /* and the frame around image */
        cairo_move_to(cri, px+2, py-8);
        cairo_line_to(cri, px+2 + (buf.width*ms) + 4, py-8);
        cairo_line_to(cri, px+2 + (buf.width*ms) + 4 , py-8-(buf.height*ms) - 4);
        cairo_line_to(cri, px+2 , py-8-(buf.height*ms) - 4);
        cairo_fill(cri);


        /* draw image*/
        cairo_translate(cri, px+4, py - 8 - (buf.height*ms) - 2);
        cairo_scale(cri, ms, ms);
        cairo_set_source_surface (cri, surface, 0, 0);
        cairo_paint(cri);

        cairo_restore(cri);

        cairo_surface_destroy(surface);

      }
    }
    else
    {
      /* just draw a patch indicating that there is images at the location */
      cairo_rectangle(cri, px-8, py-8, 16, 16);
      cairo_fill(cri);
    }
  }

}
#endif

int try_enter(dt_view_t *self)
{
  return 0;
}

static gboolean _view_map_redraw(gpointer user_data)
{
  dt_view_t *self = (dt_view_t*)user_data;
  dt_map_t *lib = (dt_map_t *)self->data;
  g_signal_emit_by_name(lib->map, "changed");
  return FALSE; // remove the function again
}

static void _view_map_changed_callback(OsmGpsMap *map, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;

  const int ts = 64;
  OsmGpsMapPoint bb[2];

  /* get bounding box coords */
  osm_gps_map_get_bbox(map, &bb[0], &bb[1]);
  float bb_0_lat = 0.0, bb_0_lon = 0.0, bb_1_lat = 0.0, bb_1_lon = 0.0;
  osm_gps_map_point_get_degrees(&bb[0], &bb_0_lat, &bb_0_lon);
  osm_gps_map_point_get_degrees(&bb[1], &bb_1_lat, &bb_1_lon);

  /* make the bounding box a little bigger to the west and south */
  float lat0 = 0.0, lon0 = 0.0, lat1 = 0.0, lon1 = 0.0;
  OsmGpsMapPoint *pt0 = osm_gps_map_point_new_degrees(0.0, 0.0), *pt1 = osm_gps_map_point_new_degrees(0.0, 0.0);
  osm_gps_map_convert_screen_to_geographic(map, 0, 0, pt0);
  osm_gps_map_convert_screen_to_geographic(map, 1.5*ts, 1.5*ts, pt1);
  osm_gps_map_point_get_degrees(pt0, &lat0, &lon0);
  osm_gps_map_point_get_degrees(pt1, &lat1, &lon1);
  osm_gps_map_point_free(pt0);
  osm_gps_map_point_free(pt1);
  double south_border = lat0 - lat1, west_border = lon1 - lon0;

  /* get map view state and store  */
  int zoom;
  float center_lat, center_lon;
  g_object_get(G_OBJECT(map), "zoom", &zoom, "latitude", &center_lat, "longitude", &center_lon, NULL);
  dt_conf_set_float("plugins/map/longitude", center_lon);
  dt_conf_set_float("plugins/map/latitude", center_lat);
  dt_conf_set_int("plugins/map/zoom", zoom);

  /* let's reset and reuse the main_query statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
  DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

  /* bind bounding box coords for the main query */
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 1, bb_0_lon - west_border);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 2, bb_1_lon);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 3, bb_0_lat);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 4, bb_1_lat - south_border);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 5, center_lat);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 6, center_lon);

  /* remove the old images */
  osm_gps_map_image_remove_all(map);
  if(lib->images)
  {
    g_slist_foreach(lib->images, (GFunc) g_free, NULL);
    g_slist_free(lib->images);
    lib->images = NULL;
  }

  /* add  all images to the map */
  gboolean needs_redraw = FALSE;
  dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, ts, ts);
  while(sqlite3_step(lib->statements.main_query) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(lib->statements.main_query, 0);
    dt_mipmap_buffer_t buf;
    dt_mipmap_cache_read_get(darktable.mipmap_cache, &buf, imgid, mip, DT_MIPMAP_BEST_EFFORT);

    if(buf.buf)
    {
      GdkPixbuf *source = NULL, *scaled = NULL;
      uint8_t *scratchmem = dt_mipmap_cache_alloc_scratchmem(darktable.mipmap_cache);
      uint8_t *buf_decompressed = dt_mipmap_cache_decompress(&buf, scratchmem);

      uint8_t *rgbbuf = (uint8_t*)malloc((buf.width+2)*(buf.height+2)*3);
      if(!rgbbuf) goto map_changed_failure;
      memset(rgbbuf, 64, (buf.width+2)*(buf.height+2)*3);
      for(int i=1; i<=buf.height; i++)
        for(int j=1; j<=buf.width; j++)
          for(int k=0; k<3; k++)
            rgbbuf[(i*(buf.width+2)+j)*3+k] = buf_decompressed[((i-1)*buf.width+j-1)*4+2-k];

      int w=ts, h=ts;
      if(buf.width < buf.height) w = (buf.width*ts)/buf.height; // portrait
      else                       h = (buf.height*ts)/buf.width; // landscape

      source = gdk_pixbuf_new_from_data(rgbbuf, GDK_COLORSPACE_RGB, FALSE, 8, (buf.width+2), (buf.height+2), (buf.width+2)*3, NULL, NULL);
      if(!source) goto map_changed_failure;
      scaled = gdk_pixbuf_scale_simple(source, w, h, GDK_INTERP_HYPER);
      if(!scaled) goto map_changed_failure;
      //TODO: add back the arrow on the left lower corner of the image, pointing to the location
      const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
      if(!cimg) goto map_changed_failure;
      dt_map_image_t *entry = (dt_map_image_t*)malloc(sizeof(dt_map_image_t));
      if(!entry)
      {
        dt_image_cache_read_release(darktable.image_cache, cimg);
        goto map_changed_failure;
      }
      entry->imgid = imgid;
      entry->image = osm_gps_map_image_add_with_alignment(map, cimg->latitude, cimg->longitude, scaled, 0, 1);
      entry->width = w;
      entry->height = h;
      lib->images = g_slist_prepend(lib->images, entry);
      dt_image_cache_read_release(darktable.image_cache, cimg);

map_changed_failure:
      if(source)
        g_object_unref(source);
      if(scaled)
        g_object_unref(scaled);
      if(rgbbuf)
        free(rgbbuf);
    }
    else
      needs_redraw = TRUE;
    dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);
  }

  // not exactly thread safe, but should be good enough for updating the display
  static int timeout_event_source = 0;
  if(needs_redraw && timeout_event_source == 0)
    timeout_event_source = g_timeout_add_seconds(1, _view_map_redraw, self); // try again in a second, maybe some pictures have loaded by then
  else
    timeout_event_source = 0;
}

static int _view_map_get_img_at_pos(dt_view_t *self, double x, double y)
{
  dt_map_t *lib = (dt_map_t*)self->data;
  GSList *iter;

  for(iter = lib->images; iter != NULL; iter = iter->next)
  {
    dt_map_image_t *entry = (dt_map_image_t*)iter->data;
    OsmGpsMapImage *image = entry->image;
    OsmGpsMapPoint *pt = (OsmGpsMapPoint*)osm_gps_map_image_get_point(image);
    gint img_x=0, img_y=0;
    osm_gps_map_convert_geographic_to_screen(lib->map, pt, &img_x, &img_y);
    if(x >= img_x && x <= img_x + entry->width && y <= img_y && y >= img_y - entry->height)
      return entry->imgid;
  }

  return 0;
}

static gboolean _view_map_motion_notify_callback(GtkWidget *w, GdkEventMotion *e, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t*)self->data;
  const int ts = 64;

  if(lib->start_drag && lib->selected_image > 0)
  {
    for(GSList *iter = lib->images; iter != NULL; iter = iter->next)
    {
      dt_map_image_t *entry = (dt_map_image_t*)iter->data;
      OsmGpsMapImage *image = entry->image;
      if(entry->imgid == lib->selected_image)
      {
        osm_gps_map_image_remove(lib->map, image);
        break;
      }
    }

    lib->start_drag = FALSE;
    GtkTargetList *targets = gtk_target_list_new(target_list_all, n_targets_all);

    dt_mipmap_buffer_t buf;
    dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, ts, ts);
    dt_mipmap_cache_read_get(darktable.mipmap_cache, &buf, lib->selected_image, mip, DT_MIPMAP_BLOCKING);

    if(buf.buf)
    {
      GdkPixbuf *scaled = NULL, *source = NULL;
      uint8_t *scratchmem = dt_mipmap_cache_alloc_scratchmem(darktable.mipmap_cache);
      uint8_t *buf_decompressed = dt_mipmap_cache_decompress(&buf, scratchmem);

      uint8_t *rgbbuf = (uint8_t*)malloc((buf.width+2)*(buf.height+2)*3);
      if(!rgbbuf) goto map_motion_failure;
      memset(rgbbuf, 64, (buf.width+2)*(buf.height+2)*3);
      for(int i=1; i<=buf.height; i++)
        for(int j=1; j<=buf.width; j++)
          for(int k=0; k<3; k++)
            rgbbuf[(i*(buf.width+2)+j)*3+k] = buf_decompressed[((i-1)*buf.width+j-1)*4+2-k];

      int w=ts, h=ts;
      if(buf.width < buf.height) w = (buf.width*ts)/buf.height; // portrait
      else                       h = (buf.height*ts)/buf.width; // landscape

      source = gdk_pixbuf_new_from_data(rgbbuf, GDK_COLORSPACE_RGB, FALSE, 8, (buf.width+2), (buf.height+2), (buf.width+2)*3, NULL, NULL);
      scaled = gdk_pixbuf_scale_simple(source, w, h, GDK_INTERP_HYPER);
      GdkDragContext * context = gtk_drag_begin(GTK_WIDGET(lib->map), targets, GDK_ACTION_COPY, 1, (GdkEvent*)e);
      gtk_drag_set_icon_pixbuf(context, scaled, 0, 0);

map_motion_failure:
      if(source)
        g_object_unref(source);
      if(scaled)
        g_object_unref(scaled);
      if(rgbbuf)
        free(rgbbuf);
    }

    dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);

    gtk_target_list_unref(targets);
    return TRUE;
  }
  return FALSE;
}

static gboolean _view_map_button_press_callback(GtkWidget *w, GdkEventButton *e, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t*)self->data;
  if(e->button == 1)
  {
    // check if the click was on an image or just some random position
    lib->selected_image = _view_map_get_img_at_pos(self, e->x, e->y);
    if(e->type == GDK_BUTTON_PRESS && lib->selected_image > 0)
    {
      lib->start_drag = TRUE;
      return TRUE;
    }
    if(e->type == GDK_2BUTTON_PRESS)
    {
      if(lib->selected_image > 0)
      {
        // open the image in darkroom
        DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, lib->selected_image);
        dt_ctl_switch_mode_to(DT_DEVELOP);
        return TRUE;
      }
      else
      {
        // zoom into that position
        float longitude, latitude;
        OsmGpsMapPoint *pt = osm_gps_map_point_new_degrees(0.0, 0.0);
        osm_gps_map_convert_screen_to_geographic(lib->map, e->x, e->y, pt);
        osm_gps_map_point_get_degrees(pt, &latitude, &longitude);
        osm_gps_map_point_free(pt);
        int zoom, max_zoom;
        g_object_get(G_OBJECT(lib->map), "zoom", &zoom, "max-zoom", &max_zoom, NULL);
        zoom = MIN(zoom+1, max_zoom);
        _view_map_center_on_location(self, longitude, latitude, zoom);
      }
      return TRUE;
    }
  }
  return FALSE;
}

void enter(dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;

  lib->selected_image = 0;
  lib->start_drag = FALSE;

  /* replace center widget */
  GtkWidget *parent = gtk_widget_get_parent(dt_ui_center(darktable.gui->ui));
  gtk_widget_hide(dt_ui_center(darktable.gui->ui));

  gtk_box_reorder_child(GTK_BOX(parent), GTK_WIDGET(lib->map), 2);

  gtk_widget_show_all(GTK_WIDGET(lib->map));

  /* setup proxy functions */
  darktable.view_manager->proxy.map.view = self;
  darktable.view_manager->proxy.map.center_on_location = _view_map_center_on_location;
  darktable.view_manager->proxy.map.show_osd = _view_map_show_osd;
  darktable.view_manager->proxy.map.set_map_source = _view_map_set_map_source;

  /* restore last zoom,location in map */
  const float lon = dt_conf_get_float("plugins/map/longitude");
  const float lat = dt_conf_get_float("plugins/map/latitude");
  const int zoom = dt_conf_get_int("plugins/map/zoom");

  osm_gps_map_set_center_and_zoom(lib->map, lat, lon, zoom);

  /* connect signal for filmstrip image activate */
  dt_control_signal_connect(darktable.signals,
                            DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE,
                            G_CALLBACK(_view_map_filmstrip_activate_callback),
                            self);
}

void leave(dt_view_t *self)
{
  /* disconnect from filmstrip image activate */
  dt_control_signal_disconnect(darktable.signals,
                               G_CALLBACK(_view_map_filmstrip_activate_callback),
                               (gpointer)self);

  dt_map_t *lib = (dt_map_t *)self->data;

  gtk_widget_hide(GTK_WIDGET(lib->map));
  gtk_widget_show_all(dt_ui_center(darktable.gui->ui));

  /* reset proxy */
  darktable.view_manager->proxy.map.view = NULL;

}

void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  // redraw center on mousemove
  dt_control_queue_redraw_center();
}

void init_key_accels(dt_view_t *self)
{
#if 0
  // Setup key accelerators in capture view...
  dt_accel_register_view(self, NC_("accel", "toggle film strip"),
                         GDK_f, GDK_CONTROL_MASK);
#endif
}

void connect_key_accels(dt_view_t *self)
{
#if 0
  GClosure *closure = g_cclosure_new(G_CALLBACK(film_strip_key_accel),
                                     (gpointer)self, NULL);
  dt_accel_connect_view(self, "toggle film strip", closure);
#endif
}


void _view_map_center_on_location(const dt_view_t *view, gdouble lon, gdouble lat, gdouble zoom)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  osm_gps_map_set_center_and_zoom(lib->map, lat, lon, zoom);
}

void _view_map_show_osd(const dt_view_t *view, gboolean enabled)
{
  dt_map_t *lib = (dt_map_t*)view->data;

  gboolean old_value = dt_conf_get_bool("plugins/map/show_map_osd");
  if(enabled == old_value)
    return;

  dt_conf_set_bool("plugins/map/show_map_osd", enabled);
  if(enabled)
    osm_gps_map_layer_add(OSM_GPS_MAP(lib->map), lib->osd);
  else
    osm_gps_map_layer_remove(OSM_GPS_MAP(lib->map), lib->osd);

  g_signal_emit_by_name(lib->map, "changed");
}

void _view_map_set_map_source(const dt_view_t *view, OsmGpsMapSource_t map_source)
{
  dt_map_t *lib = (dt_map_t*)view->data;

  const gchar *old_map_source = dt_conf_get_string("plugins/map/map_source");
  const gchar *new_map_source = osm_gps_map_source_get_friendly_name(map_source);
  if(!g_strcmp0(old_map_source, new_map_source))
    return;

  dt_conf_set_string("plugins/map/map_source", new_map_source);
  GValue value = {0,};
  g_value_init(&value, G_TYPE_INT);
  g_value_set_int(&value, map_source);
  g_object_set_property(G_OBJECT(lib->map), "map-source", &value);
  g_value_unset(&value);
}

static void _view_map_filmstrip_activate_callback(gpointer instance, gpointer user_data)
{
  dt_view_t *self = (dt_view_t*)user_data;
  dt_map_t *lib = (dt_map_t*)self->data;
  double longitude, latitude;
  int32_t imgid = 0;
  if ((imgid=dt_view_filmstrip_get_activated_imgid(darktable.view_manager))>0)
  {
    const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
    longitude = cimg->longitude;
    latitude = cimg->latitude;
    dt_image_cache_read_release(darktable.image_cache, cimg);
    if(!isnan(longitude) && !isnan(latitude))
    {
      int zoom;
      g_object_get(G_OBJECT(lib->map), "zoom", &zoom, NULL);
      _view_map_center_on_location(self, longitude, latitude, zoom); // TODO: is it better to keep the zoom level or to zoom in? 16 is a nice close up.
    }
  }
}

static void
_view_map_add_image_to_map(dt_view_t *self, int imgid, gint x, gint y)
{
  dt_map_t *lib = (dt_map_t*)self->data;
  float longitude, latitude;
  OsmGpsMapPoint *pt = osm_gps_map_point_new_degrees(0.0, 0.0);
  osm_gps_map_convert_screen_to_geographic(lib->map, x, y, pt);
  osm_gps_map_point_get_degrees(pt, &latitude, &longitude);
  osm_gps_map_point_free(pt);

  const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
  dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
  img->longitude = longitude;
  img->latitude = latitude;
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
  dt_image_cache_read_release(darktable.image_cache, cimg);
}

static void
drag_and_drop_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection_data,
                       guint target_type, guint time, gpointer data)
{
  dt_view_t *self = (dt_view_t*)data;
  dt_map_t *lib = (dt_map_t*)self->data;

  gboolean success = FALSE;

  if((selection_data != NULL) && (selection_data->length >= 0) && target_type == DND_TARGET_IMGID)
  {
    int *imgid = (int*)selection_data->data;
    if(*imgid > 0)
    {
      _view_map_add_image_to_map(self, *imgid, x, y);
      success = TRUE;
    }
    else if(*imgid == -1) // everything which is selected
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select distinct imgid from selected_images", -1, &stmt, NULL);
      while (sqlite3_step (stmt) == SQLITE_ROW)
        _view_map_add_image_to_map(self, sqlite3_column_int(stmt, 0), x, y);
      sqlite3_finalize(stmt);
      success = TRUE;
    }
  }
  gtk_drag_finish(context, success, FALSE, time);
  if(success)
    g_signal_emit_by_name(lib->map, "changed");
}

static void
_view_map_dnd_get_callback(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *selection_data,
                                guint target_type, guint time, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t*)self->data;

  g_assert (selection_data != NULL);

  int imgid = lib->selected_image;

  switch (target_type)
  {
    case DND_TARGET_IMGID:
      gtk_selection_data_set(selection_data, selection_data-> target, _DWORD, (guchar*) &imgid, sizeof(imgid));
      break;
    default: // return the location of the file as a last resort
    case DND_TARGET_URI:
    {
      gchar pathname[DT_MAX_PATH_LEN] = {0};
      dt_image_full_path(imgid, pathname, DT_MAX_PATH_LEN);
      gchar *uri = g_strdup_printf("file://%s", pathname); // TODO: should we add the host?
      gtk_selection_data_set(selection_data, selection_data-> target, _BYTE, (guchar*) uri, strlen(uri));
      g_free(uri);
      break;
    }
  }
}

static gboolean _view_map_dnd_failed_callback(GtkWidget *widget, GdkDragContext *drag_context, GtkDragResult result, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t*)self->data;

  g_signal_emit_by_name(lib->map, "changed");

  return TRUE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
