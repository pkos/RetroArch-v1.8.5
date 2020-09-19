/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2014-2017 - Jean-André Santoni
 *  Copyright (C) 2015-2018 - Andre Leiradella
 *  Copyright (C) 2018-2020 - natinusala
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <retro_miscellaneous.h>

#include <lists/file_list.h>
#include <queues/fifo_queue.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <retro_math.h>

#ifdef HAVE_THREADS
#include <rthreads/rthreads.h>
#define SLOCK_LOCK(x) slock_lock(x)
#define SLOCK_UNLOCK(x) slock_unlock(x)
#else
#define SLOCK_LOCK(x)
#define SLOCK_UNLOCK(x)
#endif

#include "gfx_widgets.h"

#include "gfx_display.h"
#include "font_driver.h"

#include "../msg_hash.h"

#include "../tasks/task_content.h"

#ifdef HAVE_CHEEVOS
#include "../cheevos-new/badges.h"
#endif

static bool widgets_inited = false;
static bool widgets_active = false;

bool gfx_widgets_active(void)
{
   return widgets_active;
}

static bool widgets_persisting = false;

void gfx_widgets_set_persistence(bool persist)
{
   widgets_persisting = persist;
}

static float msg_queue_background[16]  = COLOR_HEX_TO_FLOAT(0x3A3A3A, 1.0f);
static float msg_queue_info[16]        = COLOR_HEX_TO_FLOAT(0x12ACF8, 1.0f);

static float msg_queue_task_progress_1[16] = COLOR_HEX_TO_FLOAT(0x397869, 1.0f); /* Color of first progress bar in a task message */
static float msg_queue_task_progress_2[16] = COLOR_HEX_TO_FLOAT(0x317198, 1.0f); /* Color of second progress bar in a task message (for multiple tasks with same message) */

#if 0
static float color_task_progress_bar[16] = COLOR_HEX_TO_FLOAT(0x22B14C, 1.0f);
#endif

static uint64_t gfx_widgets_frame_count   = 0;

/* Font data */
static font_data_t *font_regular          = NULL;
static font_data_t *font_bold             = NULL;

font_data_t* gfx_widgets_get_font_regular(void)
{
   return font_regular;
}

font_data_t* gfx_widgets_get_font_bold(void)
{
   return font_bold;
}

static video_font_raster_block_t font_raster_regular;
static video_font_raster_block_t font_raster_bold;

static float gfx_widgets_pure_white[16] = {
      1.00, 1.00, 1.00, 1.00,
      1.00, 1.00, 1.00, 1.00,
      1.00, 1.00, 1.00, 1.00,
      1.00, 1.00, 1.00, 1.00,
};

float* gfx_widgets_get_pure_white(void)
{
   return gfx_widgets_pure_white;
}

/* FPS */
static char gfx_widgets_fps_text[255]  = {0};

#ifdef HAVE_CHEEVOS
/* Achievement notification */
typedef struct cheevo_popup
{
   char* title;
   uintptr_t badge;
} cheevo_popup;

#define CHEEVO_QUEUE_SIZE 8
static cheevo_popup cheevo_popup_queue[CHEEVO_QUEUE_SIZE];
static int cheevo_popup_queue_read_index = -1;
static int cheevo_popup_queue_write_index = 0;

#ifdef HAVE_THREADS
slock_t* cheevo_popup_queue_lock = NULL;
#endif

static float cheevo_unfold      = 0.0f;
static gfx_timer_t cheevo_timer;

static float cheevo_y           = 0.0f;
static unsigned cheevo_width    = 0;
static unsigned cheevo_height   = 0;

static void gfx_widgets_start_achievement_notification(void);
#endif

#ifdef HAVE_MENU
/* Load content animation */
#define ANIMATION_LOAD_CONTENT_DURATION            333

#define LOAD_CONTENT_ANIMATION_INITIAL_ICON_SIZE   320
#define LOAD_CONTENT_ANIMATION_TARGET_ICON_SIZE    240

static bool load_content_animation_running            = false;
static char *load_content_animation_content_name      = NULL;
static char *load_content_animation_playlist_name     = NULL;
static uintptr_t load_content_animation_icon          = 0;

static float load_content_animation_icon_color[16];
static float load_content_animation_icon_size;
static float load_content_animation_icon_alpha;
static float load_content_animation_fade_alpha;
static float load_content_animation_final_fade_alpha;

static gfx_timer_t load_content_animation_end_timer;

static unsigned load_content_animation_icon_size_initial;
static unsigned load_content_animation_icon_size_target;
#endif

static float gfx_widgets_backdrop_orig[16] = {
   0.00, 0.00, 0.00, 0.75,
   0.00, 0.00, 0.00, 0.75,
   0.00, 0.00, 0.00, 0.75,
   0.00, 0.00, 0.00, 0.75,
};

float* gfx_widgets_get_backdrop_orig(void)
{
   return gfx_widgets_backdrop_orig;
}

static float gfx_widgets_backdrop[16] = {
      0.00, 0.00, 0.00, 0.75,
      0.00, 0.00, 0.00, 0.75,
      0.00, 0.00, 0.00, 0.75,
      0.00, 0.00, 0.00, 0.75,
};

/* Messages queue */

typedef struct menu_widget_msg
{
   char *msg;
   char *msg_new;
   float msg_transition_animation;
   unsigned msg_len;
   unsigned duration;

   unsigned text_height;

   float offset_y;
   float alpha;

   /* Is it currently doing the fade out animation ? */
   bool dying;
   /* Has the timer expired ? if so, should be set to dying */
   bool expired;
   unsigned width;

   gfx_timer_t expiration_timer;
   bool expiration_timer_started;

   retro_task_t *task_ptr;
   /* Used to detect title change */
   char *task_title_ptr;
   /* How many tasks have used this notification? */
   uint8_t task_count;

   int8_t task_progress;
   bool task_finished;
   bool task_error;
   bool task_cancelled;
   uint32_t task_ident;

   /* Unfold animation */
   bool unfolded;
   bool unfolding;
   float unfold;

   float hourglass_rotation;
   gfx_timer_t hourglass_timer;
} menu_widget_msg_t;

static fifo_buffer_t *msg_queue                  = NULL;
static file_list_t *current_msgs                 = NULL;
static unsigned msg_queue_kill                   = 0;

/* Count of messages bound to a task in current_msgs */
static unsigned msg_queue_tasks_count            = 0;

static uintptr_t msg_queue_icon                  = 0;
static uintptr_t msg_queue_icon_outline          = 0;
static uintptr_t msg_queue_icon_rect             = 0;
static bool msg_queue_has_icons                  = false;

static gfx_animation_ctx_tag gfx_widgets_generic_tag = (uintptr_t)&widgets_active;

gfx_animation_ctx_tag gfx_widgets_get_generic_tag(void)
{
   return gfx_widgets_generic_tag;
}

/* There can only be one message animation at a time to 
 * avoid confusing users */
static bool widgets_moving                       = false;

/* Icons */
enum gfx_widgets_icon
{
   MENU_WIDGETS_ICON_PAUSED = 0,
   MENU_WIDGETS_ICON_FAST_FORWARD,
   MENU_WIDGETS_ICON_REWIND,
   MENU_WIDGETS_ICON_SLOW_MOTION,

   MENU_WIDGETS_ICON_HOURGLASS,
   MENU_WIDGETS_ICON_CHECK,

   MENU_WIDGETS_ICON_INFO,

   MENU_WIDGETS_ICON_ACHIEVEMENT,

   MENU_WIDGETS_ICON_LAST
};

static const char *gfx_widgets_icons_names[MENU_WIDGETS_ICON_LAST] = {
   "menu_pause.png",
   "menu_frameskip.png",
   "menu_rewind.png",
   "resume.png",

   "menu_hourglass.png",
   "menu_check.png",

   "menu_info.png",

   "menu_achievements.png"
};

static uintptr_t gfx_widgets_icons_textures[
   MENU_WIDGETS_ICON_LAST]                   = {0};

#ifdef HAVE_TRANSLATE
/* AI Service Overlay */
static int ai_service_overlay_state               = 0;
static unsigned ai_service_overlay_width          = 0;
static unsigned ai_service_overlay_height         = 0;
static uintptr_t ai_service_overlay_texture       = 0;
#endif

/* Libretro message */

static gfx_timer_t libretro_message_timer;
static char libretro_message[512]         = {'\0'};

/* Metrics */
#define BASE_FONT_SIZE 32.0f

static float libretro_message_alpha       = 0.0f;

static float last_scale_factor            = 0.0f;
static float msg_queue_text_scale_factor  = 0.0f;
static float widget_font_size             = 0.0f;

float gfx_widgets_get_font_size(void)
{
   return widget_font_size;
}

static unsigned simple_widget_padding     = 0;
static unsigned simple_widget_height      = 0;
static unsigned glyph_width               = 0;

unsigned gfx_widgets_get_padding(void)
{
   return simple_widget_padding;
}

unsigned gfx_widgets_get_height(void)
{
   return simple_widget_height;
}

unsigned gfx_widgets_get_glyph_width(void)
{
   return glyph_width;
}

static unsigned libretro_message_width    = 0;

static unsigned msg_queue_height;
static unsigned msg_queue_icon_size_x;
static unsigned msg_queue_icon_size_y;
static unsigned msg_queue_spacing;
static unsigned msg_queue_glyph_width;
static unsigned msg_queue_rect_start_x;
static unsigned msg_queue_internal_icon_size;
static unsigned msg_queue_internal_icon_offset;
static unsigned msg_queue_icon_offset_y;
static unsigned msg_queue_scissor_start_x;
static unsigned msg_queue_default_rect_width_menu_alive;
static unsigned msg_queue_default_rect_width;
static unsigned msg_queue_task_text_start_x;
static unsigned msg_queue_regular_padding_x;
static unsigned msg_queue_regular_text_start;
static unsigned msg_queue_regular_text_base_y;
static unsigned msg_queue_task_rect_start_x;
static unsigned msg_queue_task_hourglass_x;

/* Used for both generic and libretro messages */
static unsigned generic_message_height;

unsigned gfx_widgets_get_generic_message_height(void)
{
   return generic_message_height;
}

static unsigned divider_width_1px            = 1;

static unsigned last_video_width             = 0;
static unsigned last_video_height            = 0;

unsigned gfx_widgets_get_last_video_width(void)
{
   return last_video_width;
}

unsigned gfx_widgets_get_last_video_height(void)
{
   return last_video_height;
}

/* Widgets list */
const static gfx_widget_t* const widgets[] = {
   &gfx_widget_screenshot,
   &gfx_widget_volume,
   &gfx_widget_generic_message
};

static const size_t widgets_len = sizeof(widgets) / sizeof(widgets[0]);

static void msg_widget_msg_transition_animation_done(void *userdata)
{
   menu_widget_msg_t *msg = (menu_widget_msg_t*)userdata;

   if (msg->msg)
      free(msg->msg);
   msg->msg = NULL;

   if (msg->msg_new)
      msg->msg = strdup(msg->msg_new);

   msg->msg_transition_animation = 0.0f;
}

void gfx_widgets_msg_queue_push(
      retro_task_t *task, const char *msg,
      unsigned duration,
      char *title,
      enum message_queue_icon icon,
      enum message_queue_category category,
      unsigned prio, bool flush,
      bool menu_is_alive)
{
   menu_widget_msg_t* msg_widget = NULL;

   if (!widgets_active)
      return;

   if (fifo_write_avail(msg_queue) > 0)
   {
      /* Get current msg if it exists */
      if (task && task->frontend_userdata)
      {
         msg_widget           = (menu_widget_msg_t*)task->frontend_userdata;
         /* msg_widgets can be passed between tasks */
         msg_widget->task_ptr = task;
      }

      /* Spawn a new notification */
      if (!msg_widget)
      {
         const char *title                      = msg;

         msg_widget                             = (menu_widget_msg_t*)calloc(1, sizeof(*msg_widget));

         if (task)
            title                               = task->title;

         msg_widget->duration                   = duration;
         msg_widget->offset_y                   = 0;
         msg_widget->alpha                      = 1.0f;

         msg_widget->dying                      = false;
         msg_widget->expired                    = false;

         msg_widget->expiration_timer           = 0;
         msg_widget->task_ptr                   = task;
         msg_widget->expiration_timer_started   = false;

         msg_widget->msg_new                    = NULL;
         msg_widget->msg_transition_animation   = 0.0f;

         msg_widget->text_height                = 0;

         if (msg_queue_has_icons)
         {
            msg_widget->unfolded                = false;
            msg_widget->unfolding               = false;
            msg_widget->unfold                  = 0.0f;
         }
         else
         {
            msg_widget->unfolded                = true;
            msg_widget->unfolding               = false;
            msg_widget->unfold                  = 1.0f;
         }

         if (task)
         {
            msg_widget->msg                  = strdup(title);
            msg_widget->msg_new              = strdup(title);
            msg_widget->msg_len              = (unsigned)strlen(title);

            msg_widget->task_error           = !string_is_empty(task->error);
            msg_widget->task_cancelled       = task->cancelled;
            msg_widget->task_finished        = task->finished;
            msg_widget->task_progress        = task->progress;
            msg_widget->task_ident           = task->ident;
            msg_widget->task_title_ptr       = task->title;
            msg_widget->task_count           = 1;

            msg_widget->unfolded             = true;

            msg_widget->width                = font_driver_get_message_width(
                  font_regular, title, msg_widget->msg_len,
                  msg_queue_text_scale_factor) + simple_widget_padding/2;

            task->frontend_userdata          = msg_widget;

            msg_widget->hourglass_rotation   = 0;
         }
         else
         {
            /* Compute rect width, wrap if necessary */
            /* Single line text > two lines text > two lines 
             * text with expanded width */
            unsigned title_length   = (unsigned)strlen(title);
            char *msg               = strdup(title);
            unsigned width          = menu_is_alive 
               ? msg_queue_default_rect_width_menu_alive 
               : msg_queue_default_rect_width;
            unsigned text_width     = font_driver_get_message_width(
                  font_regular, title, title_length, msg_queue_text_scale_factor);
            msg_widget->text_height = msg_queue_text_scale_factor 
               * widget_font_size;

            /* Text is too wide, split it into two lines */
            if (text_width > width)
            {
               /* If the second line is too short, the widget may
                * look unappealing - ensure that second line is at
                * least 25% of the total width */
               if ((text_width - (text_width >> 2)) < width)
                  width = text_width - (text_width >> 2);

               word_wrap(msg, msg, (title_length * width) / text_width,
                     false, 2);

               msg_widget->text_height *= 2.5f;
            }
            else
            {
               width                      = text_width;
               msg_widget->text_height    *= 1.35f;
            }

            msg_widget->msg         = msg;
            msg_widget->msg_len     = (unsigned)strlen(msg);
            msg_widget->width       = width + simple_widget_padding/2;
         }

         fifo_write(msg_queue, &msg_widget, sizeof(msg_widget));
      }
      /* Update task info */
      else
      {
         if (msg_widget->expiration_timer_started)
         {
            gfx_timer_kill(&msg_widget->expiration_timer);
            msg_widget->expiration_timer_started = false;
         }

         if (!string_is_equal(task->title, msg_widget->msg_new))
         {
            unsigned len         = (unsigned)strlen(task->title);
            unsigned new_width   = font_driver_get_message_width(
                  font_regular, task->title, len, msg_queue_text_scale_factor);

            if (msg_widget->msg_new)
            {
               free(msg_widget->msg_new);
               msg_widget->msg_new = NULL;
            }

            msg_widget->msg_new                    = strdup(task->title);
            msg_widget->msg_len                    = len;
            msg_widget->task_title_ptr             = task->title;
            msg_widget->msg_transition_animation   = 0;

            if (!task->alternative_look)
            {
               gfx_animation_ctx_entry_t entry;

               entry.easing_enum    = EASING_OUT_QUAD;
               entry.tag            = (uintptr_t)msg_widget;
               entry.duration       = MSG_QUEUE_ANIMATION_DURATION*2;
               entry.target_value   = msg_queue_height/2.0f;
               entry.subject        = &msg_widget->msg_transition_animation;
               entry.cb             = msg_widget_msg_transition_animation_done;
               entry.userdata       = msg_widget;

               gfx_animation_push(&entry);
            }
            else
            {
               msg_widget_msg_transition_animation_done(msg_widget);
            }

            msg_widget->task_count++;

            msg_widget->width = new_width;
         }

         msg_widget->task_error        = !string_is_empty(task->error);
         msg_widget->task_cancelled    = task->cancelled;
         msg_widget->task_finished     = task->finished;
         msg_widget->task_progress     = task->progress;
      }
   }
}

static void gfx_widgets_unfold_end(void *userdata)
{
   menu_widget_msg_t *unfold = (menu_widget_msg_t*)userdata;

   unfold->unfolding         = false;
   widgets_moving            = false;
}

static void gfx_widgets_move_end(void *userdata)
{
   if (userdata)
   {
      gfx_animation_ctx_entry_t entry;
      menu_widget_msg_t *unfold = (menu_widget_msg_t*)userdata;

      entry.cb             = gfx_widgets_unfold_end;
      entry.duration       = MSG_QUEUE_ANIMATION_DURATION;
      entry.easing_enum    = EASING_OUT_QUAD;
      entry.subject        = &unfold->unfold;
      entry.tag            = (uintptr_t)unfold;
      entry.target_value   = 1.0f;
      entry.userdata       = unfold;

      gfx_animation_push(&entry);

      unfold->unfolded  = true;
      unfold->unfolding = true;
   }
   else
      widgets_moving = false;
}

static void gfx_widgets_msg_queue_expired(void *userdata)
{
   menu_widget_msg_t *msg = (menu_widget_msg_t *)userdata;

   if (msg && !msg->expired)
      msg->expired = true;
}

static void gfx_widgets_msg_queue_move(void)
{
   int i;
   float y = 0;
   /* there should always be one and only one unfolded message */
   menu_widget_msg_t *unfold  = NULL; 

   if (current_msgs->size == 0)
      return;

   for (i = (int)(current_msgs->size-1); i >= 0; i--)
   {
      menu_widget_msg_t *msg = (menu_widget_msg_t*)
         current_msgs->list[i].userdata;

      if (!msg || msg->dying)
         continue;

      y += msg_queue_height / (msg->task_ptr ? 2 : 1) + msg_queue_spacing;

      if (!msg->unfolded)
         unfold = msg;

      if (msg->offset_y != y)
      {
         gfx_animation_ctx_entry_t entry;

         entry.cb             = (i == 0) ? gfx_widgets_move_end : NULL;
         entry.duration       = MSG_QUEUE_ANIMATION_DURATION;
         entry.easing_enum    = EASING_OUT_QUAD;
         entry.subject        = &msg->offset_y;
         entry.tag            = (uintptr_t)msg;
         entry.target_value   = y;
         entry.userdata       = unfold;

         gfx_animation_push(&entry);

         widgets_moving = true;
      }
   }
}

static void gfx_widgets_msg_queue_free(menu_widget_msg_t *msg, bool touch_list)
{
   size_t i;
   gfx_animation_ctx_tag tag = (uintptr_t)msg;

   if (msg->task_ptr)
   {
      /* remove the reference the task has of ourself
         only if the task is not finished already
         (finished tasks are freed before the widget) */
      if (!msg->task_finished && !msg->task_error && !msg->task_cancelled)
         msg->task_ptr->frontend_userdata = NULL;

      /* update tasks count */
      msg_queue_tasks_count--;
   }

   /* Kill all animations */
   gfx_timer_kill(&msg->hourglass_timer);
   gfx_animation_kill_by_tag(&tag);

   /* Kill all timers */
   if (msg->expiration_timer_started)
      gfx_timer_kill(&msg->expiration_timer);

   /* Free it */
   if (msg->msg)
      free(msg->msg);

   if (msg->msg_new)
      free(msg->msg_new);

   /* Remove it from the list */
   if (touch_list)
   {
      file_list_free_userdata(current_msgs, msg_queue_kill);

      for (i = msg_queue_kill; i < current_msgs->size-1; i++)
         current_msgs->list[i] = current_msgs->list[i+1];

      current_msgs->size--;
   }

   widgets_moving = false;
}

static void gfx_widgets_msg_queue_kill_end(void *userdata)
{
   menu_widget_msg_t *msg = (menu_widget_msg_t*)
      current_msgs->list[msg_queue_kill].userdata;

   if (!msg)
      return;

   gfx_widgets_msg_queue_free(msg, true);
}

static void gfx_widgets_msg_queue_kill(unsigned idx)
{
   gfx_animation_ctx_entry_t entry;
   menu_widget_msg_t *msg = (menu_widget_msg_t*)
      current_msgs->list[idx].userdata;

   if (!msg)
      return;

   widgets_moving = true;
   msg->dying     = true;

   msg_queue_kill = idx;

   /* Drop down */
   entry.cb             = NULL;
   entry.duration       = MSG_QUEUE_ANIMATION_DURATION;
   entry.easing_enum    = EASING_OUT_QUAD;
   entry.tag            = (uintptr_t)msg;
   entry.userdata       = NULL;
   entry.subject        = &msg->offset_y;
   entry.target_value   = msg->offset_y - msg_queue_height/4;

   gfx_animation_push(&entry);

   /* Fade out */
   entry.cb             = gfx_widgets_msg_queue_kill_end;
   entry.subject        = &msg->alpha;
   entry.target_value   = 0.0f;

   gfx_animation_push(&entry);

   /* Move all messages back to their correct position */
   gfx_widgets_msg_queue_move();
}

void gfx_widgets_draw_icon(
      void *userdata,
      unsigned video_width,
      unsigned video_height,
      unsigned icon_width,
      unsigned icon_height,
      uintptr_t texture,
      float x, float y,
      unsigned width, unsigned height,
      float rotation, float scale_factor,
      float *color)
{
   gfx_display_ctx_rotate_draw_t rotate_draw;
   gfx_display_ctx_draw_t draw;
   struct video_coords coords;
   math_matrix_4x4 mymat;

   if (!texture)
      return;

   rotate_draw.matrix       = &mymat;
   rotate_draw.rotation     = rotation;
   rotate_draw.scale_x      = scale_factor;
   rotate_draw.scale_y      = scale_factor;
   rotate_draw.scale_z      = 1;
   rotate_draw.scale_enable = true;

   gfx_display_rotate_z(&rotate_draw, userdata);

   coords.vertices      = 4;
   coords.vertex        = NULL;
   coords.tex_coord     = NULL;
   coords.lut_tex_coord = NULL;
   coords.color         = color;

   draw.x               = x;
   draw.y               = height - y - icon_height;
   draw.width           = icon_width;
   draw.height          = icon_height;
   draw.scale_factor    = scale_factor;
   draw.rotation        = rotation;
   draw.coords          = &coords;
   draw.matrix_data     = &mymat;
   draw.texture         = texture;
   draw.prim_type       = GFX_DISPLAY_PRIM_TRIANGLESTRIP;
   draw.pipeline.id     = 0;

   gfx_display_draw(&draw, userdata,
         video_width, video_height);
}

#ifdef HAVE_TRANSLATE
static void gfx_widgets_draw_icon_blend(
      void *userdata,
      unsigned video_width,
      unsigned video_height,
      unsigned icon_width,
      unsigned icon_height,
      uintptr_t texture,
      float x, float y,
      unsigned width, unsigned height,
      float rotation, float scale_factor,
      float *color)
{
   gfx_display_ctx_rotate_draw_t rotate_draw;
   gfx_display_ctx_draw_t draw;
   struct video_coords coords;
   math_matrix_4x4 mymat;

   if (!texture)
      return;

   rotate_draw.matrix       = &mymat;
   rotate_draw.rotation     = rotation;
   rotate_draw.scale_x      = scale_factor;
   rotate_draw.scale_y      = scale_factor;
   rotate_draw.scale_z      = 1;
   rotate_draw.scale_enable = true;

   gfx_display_rotate_z(&rotate_draw, userdata);

   coords.vertices      = 4;
   coords.vertex        = NULL;
   coords.tex_coord     = NULL;
   coords.lut_tex_coord = NULL;
   coords.color         = color;

   draw.x               = x;
   draw.y               = height - y - icon_height;
   draw.width           = icon_width;
   draw.height          = icon_height;
   draw.scale_factor    = scale_factor;
   draw.rotation        = rotation;
   draw.coords          = &coords;
   draw.matrix_data     = &mymat;
   draw.texture         = texture;
   draw.prim_type       = GFX_DISPLAY_PRIM_TRIANGLESTRIP;
   draw.pipeline.id     = 0;

   gfx_display_draw_blend(&draw, userdata,
         video_width, video_height);
}
#endif

float gfx_widgets_get_thumbnail_scale_factor(
      const float dst_width, const float dst_height,
      const float image_width, const float image_height)
{
   float dst_ratio      = dst_width   / dst_height;
   float image_ratio    = image_width / image_height;

   if (dst_ratio > image_ratio)
      return (dst_height / image_height);
   return (dst_width / image_width);
}

static void gfx_widgets_start_msg_expiration_timer(menu_widget_msg_t *msg_widget, unsigned duration)
{
   gfx_timer_ctx_entry_t timer;
   if (msg_widget->expiration_timer_started)
      return;

   timer.cb       = gfx_widgets_msg_queue_expired;
   timer.duration = duration;
   timer.userdata = msg_widget;

   gfx_timer_start(&msg_widget->expiration_timer, &timer);

   msg_widget->expiration_timer_started = true;
}

static void gfx_widgets_hourglass_tick(void *userdata);

static void gfx_widgets_hourglass_end(void *userdata)
{
   gfx_timer_ctx_entry_t timer;
   menu_widget_msg_t *msg  = (menu_widget_msg_t*)userdata;

   msg->hourglass_rotation = 0.0f;

   timer.cb                = gfx_widgets_hourglass_tick;
   timer.duration          = HOURGLASS_INTERVAL;
   timer.userdata          = msg;

   gfx_timer_start(&msg->hourglass_timer, &timer);
}

static void gfx_widgets_hourglass_tick(void *userdata)
{
   gfx_animation_ctx_entry_t entry;
   menu_widget_msg_t    *msg = (menu_widget_msg_t*)userdata;
   gfx_animation_ctx_tag tag = (uintptr_t)msg;

   entry.easing_enum    = EASING_OUT_QUAD;
   entry.tag            = tag;
   entry.duration       = HOURGLASS_DURATION;
   entry.target_value   = -(2 * M_PI);
   entry.subject        = &msg->hourglass_rotation;
   entry.cb             = gfx_widgets_hourglass_end;
   entry.userdata       = msg;

   gfx_animation_push(&entry);
}

/* Forward declarations */
static void gfx_widgets_context_reset(bool is_threaded,
      unsigned width, unsigned height, bool fullscreen,
      const char *dir_assets, char *font_path);
static void gfx_widgets_context_destroy(void);
static void gfx_widgets_free(void);
static void gfx_widgets_layout(
      bool is_threaded, const char *dir_assets, char *font_path);
#ifdef HAVE_MENU
bool menu_driver_get_load_content_animation_data(
      uintptr_t *icon, char **playlist_name);
#endif

void gfx_widgets_iterate(
      unsigned width, unsigned height, bool fullscreen,
      const char *dir_assets, char *font_path,
      bool is_threaded)
{
   size_t i;
   float scale_factor;

   if (!widgets_active)
      return;

   /* Check whether screen dimensions or menu scale
    * factor have changed */
   scale_factor = (gfx_display_get_driver_id() == MENU_DRIVER_ID_XMB) ?
         gfx_display_get_widget_pixel_scale(width, height, fullscreen) :
               gfx_display_get_widget_dpi_scale(width, height, fullscreen);

   if ((scale_factor != last_scale_factor) ||
       (width  != last_video_width) ||
       (height != last_video_height))
   {
      last_scale_factor = scale_factor;
      last_video_width  = width;
      last_video_height = height;

      /* Note: We don't need a full context reset here
       * > Just rescale layout, and reset frame time counter */
      gfx_widgets_layout(is_threaded, dir_assets, font_path);
      video_driver_monitor_reset();
   }

   for (i = 0; i < widgets_len; i++)
   {
      const gfx_widget_t* widget = widgets[i];

      if (widget->iterate)
         widget->iterate(width, height, fullscreen, dir_assets, font_path, is_threaded);
   }

   /* Messages queue */

   /* Consume one message if available */
   if ((fifo_read_avail(msg_queue) > 0)
         && !widgets_moving 
         && (current_msgs->size < MSG_QUEUE_ONSCREEN_MAX))
   {
      menu_widget_msg_t *msg_widget;

      fifo_read(msg_queue, &msg_widget, sizeof(msg_widget));

      /* Task messages always appear from the bottom of the screen */
      if (msg_queue_tasks_count == 0 || msg_widget->task_ptr)
      {
         file_list_append(current_msgs,
            NULL,
            NULL,
            0,
            0,
            0
         );

         file_list_set_userdata(current_msgs,
               current_msgs->size-1, msg_widget);
      }
      /* Regular messages are always above tasks */
      else
      {
        unsigned idx = (unsigned)(current_msgs->size - msg_queue_tasks_count);
         file_list_insert(current_msgs,
            NULL,
            NULL,
            0,
            0,
            0,
            idx
         );

         file_list_set_userdata(current_msgs, idx, msg_widget);
      }

      /* Start expiration timer if not associated to a task */
      if (!msg_widget->task_ptr)
      {
         gfx_widgets_start_msg_expiration_timer(
               msg_widget, MSG_QUEUE_ANIMATION_DURATION * 2 
               + msg_widget->duration);
      }
      /* Else, start hourglass animation timer */
      else
      {
         msg_queue_tasks_count++;
         gfx_widgets_hourglass_end(msg_widget);
      }

      gfx_widgets_msg_queue_move();
   }

   /* Kill first expired message */
   /* Start expiration timer of dead tasks */
   for (i = 0; i < current_msgs->size ; i++)
   {
      menu_widget_msg_t *msg = (menu_widget_msg_t*)
         current_msgs->list[i].userdata;

      if (!msg)
         continue;

      if (msg->task_ptr && (msg->task_finished || msg->task_cancelled))
         gfx_widgets_start_msg_expiration_timer(msg, TASK_FINISHED_DURATION);

      if (msg->expired && !widgets_moving)
      {
         gfx_widgets_msg_queue_kill((unsigned)i);
         break;
      }
   }
}

static int gfx_widgets_draw_indicator(
      void *userdata, 
      unsigned video_width,
      unsigned video_height,
      uintptr_t icon, int y, int top_right_x_advance, 
      enum msg_hash_enums msg)
{
   unsigned width;

   gfx_display_set_alpha(gfx_widgets_backdrop_orig, DEFAULT_BACKDROP);

   if (icon)
   {
      unsigned height = simple_widget_height * 2;
      width  = height;

      gfx_display_draw_quad(userdata,
         video_width, video_height,
         top_right_x_advance - width, y,
         width, height,
         video_width, video_height,
         gfx_widgets_backdrop_orig
      );

      gfx_display_set_alpha(gfx_widgets_pure_white, 1.0f);

      gfx_display_blend_begin(userdata);
      gfx_widgets_draw_icon(
            userdata,
            video_width,
            video_height,
            width, height,
            icon, top_right_x_advance - width, y,
            video_width, video_height,
            0, 1, gfx_widgets_pure_white
            );
      gfx_display_blend_end(userdata);
   }
   else
   {
      unsigned height       = simple_widget_height;
      const char *txt       = msg_hash_to_str(msg);

      width = font_driver_get_message_width(font_regular, txt, (unsigned)strlen(txt), 1) + simple_widget_padding*2;

      gfx_display_draw_quad(userdata,
            video_width, video_height,
            top_right_x_advance - width, y,
            width, height,
            video_width, video_height,
            gfx_widgets_backdrop_orig
      );

      gfx_display_draw_text(font_regular,
         txt,
         top_right_x_advance - width + simple_widget_padding, widget_font_size + simple_widget_padding/4,
         video_width, video_height,
         0xFFFFFFFF, TEXT_ALIGN_LEFT,
         1.0f,
         false, 0, false
      );
   }

   return width;
}

static void gfx_widgets_draw_task_msg(
      menu_widget_msg_t *msg,
      void *userdata,
      unsigned video_width,
      unsigned video_height)
{
   unsigned text_color;
   unsigned bar_width;

   unsigned rect_x;
   unsigned rect_y;
   unsigned rect_width;
   unsigned rect_height;

   float *msg_queue_current_background;
   float *msg_queue_current_bar;

   bool draw_msg_new               = false;
   unsigned task_percentage_offset = 0;
   char task_percentage[256]       = {0};

   if (msg->msg_new)
      draw_msg_new = !string_is_equal(msg->msg_new, msg->msg);

   task_percentage_offset = glyph_width * (msg->task_error ? 12 : 5) + simple_widget_padding * 1.25f; /*11 = strlen("Task failed")+1 */

   if (msg->task_finished)
   {
      if (msg->task_error)
         strlcpy(task_percentage, "Task failed", sizeof(task_percentage));
      else
         strlcpy(task_percentage, " ", sizeof(task_percentage));
   }
   else if (msg->task_progress >= 0 && msg->task_progress <= 100)
      snprintf(task_percentage, sizeof(task_percentage),
            "%i%%", msg->task_progress);

   rect_width = simple_widget_padding + msg->width + task_percentage_offset;
   bar_width  = rect_width * msg->task_progress/100.0f;
   text_color = COLOR_TEXT_ALPHA(0xFFFFFF00, (unsigned)(msg->alpha*255.0f));

   /* Rect */
   if (msg->task_finished)
      if (msg->task_count == 1)
         msg_queue_current_background = msg_queue_task_progress_1;
      else
         msg_queue_current_background = msg_queue_task_progress_2;
   else
      if (msg->task_count == 1)
         msg_queue_current_background = msg_queue_background;
      else
         msg_queue_current_background = msg_queue_task_progress_1;

   rect_x      = msg_queue_rect_start_x - msg_queue_icon_size_x;
   rect_y      = video_height - msg->offset_y;
   rect_height = msg_queue_height/2;

   gfx_display_set_alpha(msg_queue_current_background, msg->alpha);
   gfx_display_draw_quad(userdata,
         video_width, video_height,
         rect_x, rect_y,
         rect_width, rect_height,
         video_width, video_height,
         msg_queue_current_background
         );

   /* Progress bar */
   if (!msg->task_finished && msg->task_progress >= 0 && msg->task_progress <= 100)
   {
      if (msg->task_count == 1)
         msg_queue_current_bar = msg_queue_task_progress_1;
      else
         msg_queue_current_bar = msg_queue_task_progress_2;

      gfx_display_set_alpha(msg_queue_current_bar, 1.0f);
      gfx_display_draw_quad(userdata,
            video_width, video_height,
            msg_queue_task_rect_start_x, video_height - msg->offset_y,
            bar_width, rect_height,
            video_width, video_height,
            msg_queue_current_bar
            );
   }

   /* Icon */
   gfx_display_set_alpha(gfx_widgets_pure_white, msg->alpha);
   gfx_display_blend_begin(userdata);
   gfx_widgets_draw_icon(
         userdata,
         video_width,
         video_height,
         msg_queue_height/2,
         msg_queue_height/2,
         gfx_widgets_icons_textures[msg->task_finished ? MENU_WIDGETS_ICON_CHECK : MENU_WIDGETS_ICON_HOURGLASS],
         msg_queue_task_hourglass_x,
         video_height - msg->offset_y,
         video_width,
         video_height,
         msg->task_finished ? 0 : msg->hourglass_rotation,
         1, gfx_widgets_pure_white);
   gfx_display_blend_end(userdata);

   /* Text */
   if (draw_msg_new)
   {
      font_driver_flush(video_width, video_height, font_regular);
      font_raster_regular.carr.coords.vertices  = 0;

      gfx_display_scissor_begin(userdata,
            video_width, video_height,
            rect_x, rect_y, rect_width, rect_height);
      gfx_display_draw_text(font_regular,
         msg->msg_new,
         msg_queue_task_text_start_x,
         video_height - msg->offset_y + msg_queue_text_scale_factor * widget_font_size + msg_queue_height/4 - widget_font_size/2.25f - msg_queue_height/2 + msg->msg_transition_animation,
         video_width, video_height,
         text_color,
         TEXT_ALIGN_LEFT,
         msg_queue_text_scale_factor,
         false,
         0,
         true
      );
   }

   gfx_display_draw_text(font_regular,
      msg->msg,
      msg_queue_task_text_start_x,
      video_height - msg->offset_y + msg_queue_text_scale_factor * widget_font_size + msg_queue_height/4 - widget_font_size/2.25f + msg->msg_transition_animation,
      video_width, video_height,
      text_color,
      TEXT_ALIGN_LEFT,
      msg_queue_text_scale_factor,
      false,
      0,
      true
   );

   if (draw_msg_new)
   {
      font_driver_flush(video_width, video_height, font_regular);
      font_raster_regular.carr.coords.vertices  = 0;

      gfx_display_scissor_end(userdata,
            video_width, video_height);
   }

   /* Progress text */
   text_color = COLOR_TEXT_ALPHA(0xFFFFFF00, (unsigned)(msg->alpha/2*255.0f));
   gfx_display_draw_text(font_regular,
      task_percentage,
      msg_queue_rect_start_x - msg_queue_icon_size_x + rect_width - msg_queue_glyph_width,
      video_height - msg->offset_y + msg_queue_text_scale_factor * widget_font_size + msg_queue_height/4 - widget_font_size/2.25f,
      video_width, video_height,
      text_color,
      TEXT_ALIGN_RIGHT,
      msg_queue_text_scale_factor,
      false,
      0,
      true
   );
}

static void gfx_widgets_draw_regular_msg(
      menu_widget_msg_t *msg,
      void *userdata,
      unsigned video_width,
      unsigned video_height)
{
   unsigned bar_width;
   unsigned text_color;
   uintptr_t icon        = 0;

   if (!icon)
      icon = gfx_widgets_icons_textures[MENU_WIDGETS_ICON_INFO]; /* TODO: Real icon logic here */

   /* Icon */
   gfx_display_set_alpha(msg_queue_info, msg->alpha);
   gfx_display_set_alpha(gfx_widgets_pure_white, msg->alpha);
   gfx_display_set_alpha(msg_queue_background, msg->alpha);

   if (!msg->unfolded || msg->unfolding)
   {
      font_driver_flush(video_width, video_height, font_regular);
      font_driver_flush(video_width, video_height, font_bold);

      font_raster_regular.carr.coords.vertices  = 0;
      font_raster_bold.carr.coords.vertices     = 0;

      gfx_display_scissor_begin(userdata,
            video_width, video_height,
            msg_queue_scissor_start_x, 0,
            (msg_queue_scissor_start_x + msg->width - simple_widget_padding*2) * msg->unfold, video_height);
   }

   if (msg_queue_has_icons)
   {
      gfx_display_blend_begin(userdata);
      /* (int) cast is to be consistent with the rect drawing and prevent alignment
      * issues, don't remove it */
      gfx_widgets_draw_icon(
            userdata,
            video_width,
            video_height,
            msg_queue_icon_size_x, msg_queue_icon_size_y,
            msg_queue_icon_rect, msg_queue_spacing, (int)(video_height - msg->offset_y - msg_queue_icon_offset_y),
            video_width, video_height, 
            0, 1, msg_queue_background);

      gfx_display_blend_end(userdata);
   }

   /* Background */
   bar_width = simple_widget_padding + msg->width;

   gfx_display_draw_quad(userdata,
         video_width, video_height,
         msg_queue_rect_start_x, video_height - msg->offset_y,
         bar_width, msg_queue_height,
         video_width, video_height,
         msg_queue_background
         );

   /* Text */
   text_color = COLOR_TEXT_ALPHA(0xFFFFFF00, (unsigned)(msg->alpha*255.0f));

   gfx_display_draw_text(font_regular,
      msg->msg,
      msg_queue_regular_text_start - ((1.0f-msg->unfold) * msg->width/2),
      video_height - msg->offset_y + msg_queue_regular_text_base_y - msg->text_height/2,
      video_width, video_height,
      text_color,
      TEXT_ALIGN_LEFT,
      msg_queue_text_scale_factor, false, 0, true
   );

   if (!msg->unfolded || msg->unfolding)
   {
      font_driver_flush(video_width, video_height, font_regular);
      font_driver_flush(video_width, video_height, font_bold);

      font_raster_regular.carr.coords.vertices  = 0;
      font_raster_bold.carr.coords.vertices     = 0;

      gfx_display_scissor_end(userdata,
            video_width, video_height);
   }

   if (msg_queue_has_icons)
   {
      gfx_display_blend_begin(userdata);

      gfx_widgets_draw_icon(
            userdata,
            video_width,
            video_height,
            msg_queue_icon_size_x, msg_queue_icon_size_y,
            msg_queue_icon, msg_queue_spacing, video_height 
            - msg->offset_y - msg_queue_icon_offset_y, 
            video_width, video_height,
            0, 1, msg_queue_info);

      gfx_widgets_draw_icon(
            userdata,
            video_width,
            video_height,
            msg_queue_icon_size_x, msg_queue_icon_size_y,
            msg_queue_icon_outline, msg_queue_spacing, video_height - msg->offset_y - msg_queue_icon_offset_y, 
            video_width, video_height,
            0, 1, gfx_widgets_pure_white);

      gfx_widgets_draw_icon(
            userdata,
            video_width,
            video_height,
            msg_queue_internal_icon_size, msg_queue_internal_icon_size,
            icon, msg_queue_spacing + msg_queue_internal_icon_offset,
            video_height - msg->offset_y - msg_queue_icon_offset_y + msg_queue_internal_icon_offset, 
            video_width, video_height,
            0, 1, gfx_widgets_pure_white);

      gfx_display_blend_end(userdata);
   }
}

static void gfx_widgets_draw_backdrop(
      void *userdata,
      unsigned video_width,
      unsigned video_height,
      float alpha)
{
   gfx_display_set_alpha(gfx_widgets_backdrop, alpha);
   gfx_display_draw_quad(userdata,
         video_width, video_height, 0, 0,
         video_width, video_height, video_width, video_height,
         gfx_widgets_backdrop);
}

static void gfx_widgets_draw_load_content_animation(
      void *userdata,
      unsigned video_width,
      unsigned video_height)
{
#ifdef HAVE_MENU
   /* TODO: change metrics? */
   int icon_size         = (int)load_content_animation_icon_size;
   uint32_t text_alpha   = load_content_animation_fade_alpha * 255.0f;
   uint32_t text_color   = COLOR_TEXT_ALPHA(0xB8B8B800, text_alpha);
   unsigned text_offset  = -25 * last_scale_factor * load_content_animation_fade_alpha;
   float *icon_color     = load_content_animation_icon_color;

   /* Fade out */
   gfx_widgets_draw_backdrop(userdata,
         video_width, video_height, load_content_animation_fade_alpha);

   /* Icon */
   gfx_display_set_alpha(icon_color, load_content_animation_icon_alpha);
   gfx_display_blend_begin(userdata);
   gfx_widgets_draw_icon(
         userdata,
         video_width,
         video_height,
         icon_size,
         icon_size, load_content_animation_icon,
         video_width  / 2 - icon_size/2,
         video_height / 2 - icon_size/2,
         video_width,
         video_height,
         0, 1, icon_color
         );
   gfx_display_blend_end(userdata);

   /* Text */
   gfx_display_draw_text(font_bold,
      load_content_animation_content_name,
      video_width  / 2,
      video_height / 2 + (175 + 25) * last_scale_factor + text_offset,
      video_width,
      video_height,
      text_color,
      TEXT_ALIGN_CENTER,
      1,
      false,
      0,
      false
   );

   /* Flush text layer */
   font_driver_flush(video_width, video_height, font_regular);
   font_driver_flush(video_width, video_height, font_bold);

   font_raster_regular.carr.coords.vertices = 0;
   font_raster_bold.carr.coords.vertices = 0;

   /* Everything disappears */
   gfx_widgets_draw_backdrop(userdata,
         video_width, video_height,
         load_content_animation_final_fade_alpha);
#endif
}

void gfx_widgets_frame(void *data)
{
   /* (Pointless) optimisation: allocating these
    * costs nothing, so do it *before* the
    * 'widgets_active' check... */
   size_t i;
   video_frame_info_t *video_info;
   bool framecount_show;
   bool memory_show;
   void *userdata;
   unsigned video_width;
   unsigned video_height;
   bool widgets_is_paused;
   bool fps_show;
   bool widgets_is_fastforwarding;
   bool widgets_is_rewinding;
   bool runloop_is_slowmotion;
   int top_right_x_advance;
   int scissor_me_timbers;

   if (!widgets_active)
      return;

   /* ...but assigning them costs a tiny amount,
    * so do it *after* the 'widgets_active' check */
   video_info                = (video_frame_info_t*)data;
   framecount_show           = video_info->framecount_show;
   memory_show               = video_info->memory_show;
   userdata                  = video_info->userdata;
   video_width               = video_info->width;
   video_height              = video_info->height;
   widgets_is_paused         = video_info->widgets_is_paused;
   fps_show                  = video_info->fps_show;
   widgets_is_fastforwarding = video_info->widgets_is_fast_forwarding;
   widgets_is_rewinding      = video_info->widgets_is_rewinding;
   runloop_is_slowmotion     = video_info->runloop_is_slowmotion;
   top_right_x_advance       = video_width;
   scissor_me_timbers        = 0;

   gfx_widgets_frame_count++;

   gfx_display_set_viewport(video_width, video_height);

   /* Font setup */
   font_driver_bind_block(font_regular, &font_raster_regular);
   font_driver_bind_block(font_bold, &font_raster_bold);

   font_raster_regular.carr.coords.vertices = 0;
   font_raster_bold.carr.coords.vertices    = 0;

#ifdef HAVE_TRANSLATE
   /* AI Service overlay */
   if (ai_service_overlay_state > 0)
   {
      float outline_color[16] = {
      0.00, 1.00, 0.00, 1.00,
      0.00, 1.00, 0.00, 1.00,
      0.00, 1.00, 0.00, 1.00,
      0.00, 1.00, 0.00, 1.00,
      };
      gfx_display_set_alpha(gfx_widgets_pure_white, 1.0f);

      gfx_widgets_draw_icon_blend(
            userdata,
            video_width,
            video_height,
            video_width, video_height,
            ai_service_overlay_texture,
            0, 0,
            video_width, video_height,
            0, 1, gfx_widgets_pure_white
            );
      /* top line */
      gfx_display_draw_quad(userdata,
            video_width, video_height,
            0, 0,
            video_width, divider_width_1px,
            video_width, video_height,
            outline_color
            );
      /* bottom line */
      gfx_display_draw_quad(userdata,
            video_width, video_height,
            0, video_height - divider_width_1px,
            video_width, divider_width_1px,
            video_width, video_height,
            outline_color
            );
      /* left line */
      gfx_display_draw_quad(userdata,
            video_width,
            video_height,
            0, 0,
            divider_width_1px, video_height,
            video_width, video_height,
            outline_color
            );
      /* right line */
      gfx_display_draw_quad(userdata,
            video_width, video_height,
            video_width - divider_width_1px, 0,
            divider_width_1px, video_height,
            video_width, video_height,
            outline_color
            );

      if (ai_service_overlay_state == 2)
          ai_service_overlay_state = 3;
   }
#endif

   /* Libretro message */
   if (libretro_message_alpha > 0.0f)
   {
      unsigned text_color = COLOR_TEXT_ALPHA(0xffffffff, (unsigned)(libretro_message_alpha*255.0f));
      gfx_display_set_alpha(gfx_widgets_backdrop_orig, libretro_message_alpha);

      gfx_display_draw_quad(userdata,
            video_width, video_height,
            0, video_height - generic_message_height,
            libretro_message_width, generic_message_height,
            video_width, video_height,
            gfx_widgets_backdrop_orig);

      gfx_display_draw_text(font_regular, libretro_message,
         simple_widget_padding,
         video_height - generic_message_height/2 + widget_font_size/4,
         video_width, video_height,
         text_color, TEXT_ALIGN_LEFT,
         1, false, 0, false);
   }

#ifdef HAVE_CHEEVOS
   /* Achievement notification */
   if (cheevo_popup_queue_read_index >= 0 && cheevo_popup_queue[cheevo_popup_queue_read_index].title)
   {
      SLOCK_LOCK(cheevo_popup_queue_lock);

      if (cheevo_popup_queue[cheevo_popup_queue_read_index].title)
      {
         unsigned unfold_offet = ((1.0f - cheevo_unfold) * cheevo_width / 2);

         gfx_display_set_alpha(gfx_widgets_backdrop_orig, DEFAULT_BACKDROP);
         gfx_display_set_alpha(gfx_widgets_pure_white, 1.0f);

         /* Default icon */
         if (!cheevo_popup_queue[cheevo_popup_queue_read_index].badge)
         {
            /* Backdrop */
            gfx_display_draw_quad(userdata,
                  video_width, video_height,
                  0, (int)cheevo_y,
                  cheevo_height, cheevo_height,
                  video_width, video_height,
                  gfx_widgets_backdrop_orig);

            /* Icon */
            if (gfx_widgets_icons_textures[MENU_WIDGETS_ICON_ACHIEVEMENT])
            {
               gfx_display_blend_begin(userdata);
               gfx_widgets_draw_icon(
                     userdata,
                     video_width,
                     video_height,
                     cheevo_height, cheevo_height,
                     gfx_widgets_icons_textures[MENU_WIDGETS_ICON_ACHIEVEMENT], 0, cheevo_y,
                     video_width, video_height, 0, 1, gfx_widgets_pure_white);
               gfx_display_blend_end(userdata);
            }
         }
         /* Badge */
         else
         {
            gfx_widgets_draw_icon(
                  userdata,
                  video_width,
                  video_height,
                  cheevo_height, cheevo_height,
                  cheevo_popup_queue[cheevo_popup_queue_read_index].badge,
                  0,
                  cheevo_y,
                  video_width, video_height, 0, 1, gfx_widgets_pure_white);
         }

         /* I _think_ cheevo_unfold changes in another thread */
         scissor_me_timbers = (fabs(cheevo_unfold - 1.0f) > 0.01);
         if (scissor_me_timbers)
            gfx_display_scissor_begin(userdata,
                  video_width,
                  video_height,
                  cheevo_height, 0,
                  (unsigned)((float)(cheevo_width)*cheevo_unfold),
                  cheevo_height);

         /* Backdrop */
         gfx_display_draw_quad(userdata,
               video_width, video_height,
               cheevo_height, (int)cheevo_y,
               cheevo_width, cheevo_height,
               video_width, video_height,
               gfx_widgets_backdrop_orig);

         /* Title */
         gfx_display_draw_text(font_regular,
            msg_hash_to_str(MSG_ACHIEVEMENT_UNLOCKED),
            cheevo_height + simple_widget_padding - unfold_offet,
            widget_font_size * 1.9f + cheevo_y,
            video_width, video_height,
            TEXT_COLOR_FAINT,
            TEXT_ALIGN_LEFT,
            1, false, 0, true
         );

         /* Title */

         /* TODO: is a ticker necessary ? */

         gfx_display_draw_text(font_regular,
            cheevo_popup_queue[cheevo_popup_queue_read_index].title,
            cheevo_height + simple_widget_padding - unfold_offet, widget_font_size * 2.9f + cheevo_y,
            video_width, video_height,
            TEXT_COLOR_INFO,
            TEXT_ALIGN_LEFT,
            1, false, 0, true
         );

         if (scissor_me_timbers)
         {
            font_driver_flush(video_width, video_height, font_regular);
            font_raster_regular.carr.coords.vertices = 0;
            gfx_display_scissor_end(userdata,
                  video_width, video_height);
         }
      }

      SLOCK_UNLOCK(cheevo_popup_queue_lock);
   }
#endif

   /* Draw all messages */
   for (i = 0; i < current_msgs->size; i++)
   {
      menu_widget_msg_t *msg = (menu_widget_msg_t*)current_msgs->list[i].userdata;

      if (!msg)
         continue;

      if (msg->task_ptr)
         gfx_widgets_draw_task_msg(msg, userdata,
               video_width, video_height);
      else
         gfx_widgets_draw_regular_msg(msg, userdata,
               video_width, video_height);
   }

   /* FPS Counter */
   if (     fps_show 
         || framecount_show
         || memory_show
         )
   {
      const char *text      = *gfx_widgets_fps_text == '\0' ? "N/A" : gfx_widgets_fps_text;

      int text_width        = font_driver_get_message_width(font_regular, text, (unsigned)strlen(text), 1.0f);
      int total_width       = text_width + simple_widget_padding * 2;

      int fps_text_x        = top_right_x_advance - simple_widget_padding - text_width;
      /* Ensure that left hand side of text does
       * not bleed off the edge of the screen */
      fps_text_x            = (fps_text_x < 0) ? 0 : fps_text_x;

      gfx_display_set_alpha(gfx_widgets_backdrop_orig, DEFAULT_BACKDROP);

      gfx_display_draw_quad(userdata,
            video_width,
            video_height,
            top_right_x_advance - total_width, 0,
            total_width, simple_widget_height,
            video_width, video_height,
            gfx_widgets_backdrop_orig
            );

      gfx_display_draw_text(font_regular,
         text,
         fps_text_x, widget_font_size + simple_widget_padding/4,
         video_width, video_height,
         0xFFFFFFFF,
         TEXT_ALIGN_LEFT,
         1, false,0, true
      );
   }

   /* Indicators */
   if (widgets_is_paused)
      top_right_x_advance -= gfx_widgets_draw_indicator(
            userdata,
            video_width,
            video_height,
            gfx_widgets_icons_textures[MENU_WIDGETS_ICON_PAUSED], (fps_show ? simple_widget_height : 0), top_right_x_advance,
            MSG_PAUSED);

   if (widgets_is_fastforwarding)
      top_right_x_advance -= gfx_widgets_draw_indicator(
            userdata,
            video_width,
            video_height,
            gfx_widgets_icons_textures[MENU_WIDGETS_ICON_FAST_FORWARD], (fps_show ? simple_widget_height : 0), top_right_x_advance,
            MSG_PAUSED);

   if (widgets_is_rewinding)
      top_right_x_advance -= gfx_widgets_draw_indicator(
            userdata,
            video_width,
            video_height,
            gfx_widgets_icons_textures[MENU_WIDGETS_ICON_REWIND], (fps_show ? simple_widget_height : 0), top_right_x_advance,
            MSG_REWINDING);

   if (runloop_is_slowmotion)
      top_right_x_advance -= gfx_widgets_draw_indicator(
            userdata,
            video_width,
            video_height,
            gfx_widgets_icons_textures[MENU_WIDGETS_ICON_SLOW_MOTION], (fps_show ? simple_widget_height : 0), top_right_x_advance,
            MSG_SLOW_MOTION);

   for (i = 0; i < widgets_len; i++)
   {
      const gfx_widget_t* widget = widgets[i];

      if (widget->frame)
         widget->frame(data);
   }

#ifdef HAVE_MENU
   /* Load content animation */
   if (load_content_animation_running)
      gfx_widgets_draw_load_content_animation(userdata,
            video_width, video_height);
   else
#endif
   {
      font_driver_flush(video_width, video_height, font_regular);
      font_driver_flush(video_width, video_height, font_bold);

      font_raster_regular.carr.coords.vertices = 0;
      font_raster_bold.carr.coords.vertices = 0;
   }

   gfx_display_unset_viewport(video_width, video_height);
}

bool gfx_widgets_init(
      bool video_is_threaded,
      unsigned width, unsigned height, bool fullscreen,
      const char *dir_assets, char *font_path)
{
   if (!gfx_display_init_first_driver(video_is_threaded))
      goto error;

   if (!widgets_inited)
   {
      size_t i;

      gfx_widgets_frame_count = 0;

      for (i = 0; i < widgets_len; i++)
      {
         const gfx_widget_t* widget = widgets[i];

         if (widget->init)
            widget->init(video_is_threaded, fullscreen);
      }

      msg_queue = fifo_new(MSG_QUEUE_PENDING_MAX * sizeof(menu_widget_msg_t*));

      if (!msg_queue)
         goto error;

      current_msgs = (file_list_t*)calloc(1, sizeof(file_list_t));

      if (!current_msgs)
         goto error;

      if (!file_list_reserve(current_msgs, MSG_QUEUE_ONSCREEN_MAX))
         goto error;

      widgets_inited = true;
   }

   gfx_widgets_context_reset(video_is_threaded,
         width, height, fullscreen,
         dir_assets, font_path);

   widgets_active = true;

   return true;

error:
   gfx_widgets_free();
   return false;
}

void gfx_widgets_deinit(void)
{
   if (!widgets_inited)
      return;

   widgets_active = false;
   gfx_widgets_context_destroy();

   if (!widgets_persisting)
      gfx_widgets_free();
}

static void gfx_widgets_layout(
      bool is_threaded, const char *dir_assets, char *font_path)
{
   size_t i;
   int font_height = 0;

   /* Base font size must be determined first
    * > Note that size must be at least 2,
    *   otherwise gfx_display_font_file() will
    *   generate a heap-buffer-overflow */
   widget_font_size = BASE_FONT_SIZE * last_scale_factor;
   widget_font_size = (widget_font_size > 2.0f) ? widget_font_size : 2.0f;

   /* Initialise fonts */

   /* > Free existing */
   if (font_regular)
   {
      gfx_display_font_free(font_regular);
      font_regular = NULL;
   }
   if (font_bold)
   {
      gfx_display_font_free(font_bold);
      font_bold = NULL;
   }

   /* > Create new */
   if (string_is_empty(font_path))
   {
      char ozone_path[PATH_MAX_LENGTH];
      char font_path[PATH_MAX_LENGTH];

      ozone_path[0] = '\0';
      font_path[0]  = '\0';

      /* Base path */
      fill_pathname_join(ozone_path, dir_assets, "ozone", sizeof(ozone_path));

      /* Create regular font */
      fill_pathname_join(font_path, ozone_path, "regular.ttf", sizeof(font_path));
      font_regular = gfx_display_font_file(font_path, widget_font_size, is_threaded);

      /* Create bold font */
      fill_pathname_join(font_path, ozone_path, "bold.ttf", sizeof(font_path));
      font_bold = gfx_display_font_file(font_path, widget_font_size, is_threaded);
   }
   else
   {
      /* Load fonts from user-supplied path */
      font_regular = gfx_display_font_file(font_path, widget_font_size, is_threaded);
      font_bold    = gfx_display_font_file(font_path, widget_font_size, is_threaded);
   }

   /* > Get actual font size */
   font_height = font_driver_get_line_height(font_regular, 1.0f);
   if (font_height > 0)
      widget_font_size = (float)font_height;

   /* Calculate dimensions */
   simple_widget_padding            = widget_font_size * 2.0f/3.0f;
   simple_widget_height             = widget_font_size + simple_widget_padding;
   glyph_width                      = font_driver_get_message_width(font_regular, "a", 1, 1);

   msg_queue_height                 = widget_font_size * 2.5f;

   if (msg_queue_has_icons)
   {
      msg_queue_icon_size_y         = msg_queue_height * 1.2347826087f; /* original image is 280x284 */
      msg_queue_icon_size_x         = 0.98591549295f * msg_queue_icon_size_y;
   }
   else
   {
      msg_queue_icon_size_x         = 0;
      msg_queue_icon_size_y         = 0;
   }

   msg_queue_text_scale_factor      = 0.69f;
   msg_queue_spacing                = msg_queue_height / 3;
   msg_queue_glyph_width            = glyph_width * msg_queue_text_scale_factor;
   msg_queue_rect_start_x           = msg_queue_spacing + msg_queue_icon_size_x;
   msg_queue_internal_icon_size     = msg_queue_icon_size_y;
   msg_queue_internal_icon_offset   = (msg_queue_icon_size_y - msg_queue_internal_icon_size)/2;
   msg_queue_icon_offset_y          = (msg_queue_icon_size_y - msg_queue_height)/2;
   msg_queue_scissor_start_x        = msg_queue_spacing + msg_queue_icon_size_x - (msg_queue_icon_size_x * 0.28928571428f);

   if (msg_queue_has_icons)
      msg_queue_regular_padding_x   = simple_widget_padding/2;
   else
      msg_queue_regular_padding_x   = simple_widget_padding;

   msg_queue_task_rect_start_x      = msg_queue_rect_start_x - msg_queue_icon_size_x;

   msg_queue_task_text_start_x      = msg_queue_task_rect_start_x + msg_queue_height/2;

   if (!gfx_widgets_icons_textures[MENU_WIDGETS_ICON_HOURGLASS])
      msg_queue_task_text_start_x   -= msg_queue_glyph_width*2;

   msg_queue_regular_text_start     = msg_queue_rect_start_x + msg_queue_regular_padding_x;
   msg_queue_regular_text_base_y    = widget_font_size * msg_queue_text_scale_factor + msg_queue_height/2;

   msg_queue_task_hourglass_x       = msg_queue_rect_start_x - msg_queue_icon_size_x;

   generic_message_height           = widget_font_size * 2;

   msg_queue_default_rect_width_menu_alive = msg_queue_glyph_width * 40;
   msg_queue_default_rect_width            = last_video_width - msg_queue_regular_text_start - (2 * simple_widget_padding);

#ifdef HAVE_MENU
   load_content_animation_icon_size_initial = LOAD_CONTENT_ANIMATION_INITIAL_ICON_SIZE * last_scale_factor;
   load_content_animation_icon_size_target  = LOAD_CONTENT_ANIMATION_TARGET_ICON_SIZE * last_scale_factor;
#endif

   divider_width_1px    = 1;
   if (last_scale_factor > 1.0f)
      divider_width_1px = (unsigned)(last_scale_factor + 0.5f);

   for (i = 0; i < widgets_len; i++)
   {
      const gfx_widget_t* widget = widgets[i];

      if (widget->layout)
         widget->layout(is_threaded, dir_assets, font_path);
   }
}

static void gfx_widgets_context_reset(bool is_threaded,
      unsigned width, unsigned height, bool fullscreen,
      const char *dir_assets, char *font_path)
{
   size_t i;
   char xmb_path[PATH_MAX_LENGTH];
   char monochrome_png_path[PATH_MAX_LENGTH];
   char gfx_widgets_path[PATH_MAX_LENGTH];
   char theme_path[PATH_MAX_LENGTH];

   xmb_path[0]            = '\0';
   monochrome_png_path[0] = '\0';
   gfx_widgets_path[0]    = '\0';
   theme_path[0]          = '\0';

   /* Textures paths */
   fill_pathname_join(
      gfx_widgets_path,
      dir_assets,
      "menu_widgets",
      sizeof(gfx_widgets_path)
   );

   fill_pathname_join(
      xmb_path,
      dir_assets,
      "xmb",
      sizeof(xmb_path)
   );

   /* Monochrome */
   fill_pathname_join(
      theme_path,
      xmb_path,
      "monochrome",
      sizeof(theme_path)
   );

   fill_pathname_join(
      monochrome_png_path,
      theme_path,
      "png",
      sizeof(monochrome_png_path)
   );

   /* Load textures */
   /* Icons */
   for (i = 0; i < MENU_WIDGETS_ICON_LAST; i++)
   {
      gfx_display_reset_textures_list(gfx_widgets_icons_names[i], monochrome_png_path, &gfx_widgets_icons_textures[i], TEXTURE_FILTER_MIPMAP_LINEAR, NULL, NULL);
   }

   /* Message queue */
   gfx_display_reset_textures_list("msg_queue_icon.png", gfx_widgets_path, &msg_queue_icon, TEXTURE_FILTER_LINEAR, NULL, NULL);
   gfx_display_reset_textures_list("msg_queue_icon_outline.png", gfx_widgets_path, &msg_queue_icon_outline, TEXTURE_FILTER_LINEAR, NULL, NULL);
   gfx_display_reset_textures_list("msg_queue_icon_rect.png", gfx_widgets_path, &msg_queue_icon_rect, TEXTURE_FILTER_NEAREST, NULL, NULL);

   msg_queue_has_icons = msg_queue_icon && msg_queue_icon_outline && msg_queue_icon_rect;

   for (i = 0; i < widgets_len; i++)
   {
      const gfx_widget_t* widget = widgets[i];

      if (widget->context_reset)
         widget->context_reset(is_threaded, width, height, fullscreen, dir_assets, font_path, monochrome_png_path, gfx_widgets_path);
   }

   /* Update scaling/dimensions */
   last_video_width  = width;
   last_video_height = height;
   last_scale_factor = (gfx_display_get_driver_id() == MENU_DRIVER_ID_XMB) ?
         gfx_display_get_widget_pixel_scale(last_video_width, last_video_height, fullscreen) :
               gfx_display_get_widget_dpi_scale(last_video_width, last_video_height, fullscreen);

   gfx_widgets_layout(is_threaded, dir_assets, font_path);
   video_driver_monitor_reset();
}

static void gfx_widgets_context_destroy(void)
{
   size_t i;

   for (i = 0; i < widgets_len; i++)
   {
      const gfx_widget_t* widget = widgets[i];

      if (widget->context_destroy)
         widget->context_destroy();
   }

   /* TODO: Dismiss onscreen notifications that have been freed */

   /* Textures */
   for (i = 0; i < MENU_WIDGETS_ICON_LAST; i++)
      video_driver_texture_unload(&gfx_widgets_icons_textures[i]);

   video_driver_texture_unload(&msg_queue_icon);
   video_driver_texture_unload(&msg_queue_icon_outline);
   video_driver_texture_unload(&msg_queue_icon_rect);

   msg_queue_icon         = 0;
   msg_queue_icon_outline = 0;
   msg_queue_icon_rect    = 0;

   /* Fonts */
   if (font_regular)
      gfx_display_font_free(font_regular);
   if (font_bold)
      gfx_display_font_free(font_bold);

   font_regular = NULL;
   font_bold    = NULL;
}

#ifdef HAVE_CHEEVOS
static void gfx_widgets_achievement_free_current(void)
{
   if (cheevo_popup_queue[cheevo_popup_queue_read_index].title)
   {
      free(cheevo_popup_queue[cheevo_popup_queue_read_index].title);
      cheevo_popup_queue[cheevo_popup_queue_read_index].title = NULL;
   }

   if (cheevo_popup_queue[cheevo_popup_queue_read_index].badge)
   {
      video_driver_texture_unload(&cheevo_popup_queue[cheevo_popup_queue_read_index].badge);
      cheevo_popup_queue[cheevo_popup_queue_read_index].badge = 0;
   }

   cheevo_popup_queue_read_index = (cheevo_popup_queue_read_index + 1) % CHEEVO_QUEUE_SIZE;
}

static void gfx_widgets_achievement_next(void* userdata)
{
   SLOCK_LOCK(cheevo_popup_queue_lock);

   gfx_widgets_achievement_free_current();

   /* start the next popup (if present) */
   if (cheevo_popup_queue[cheevo_popup_queue_read_index].title)
      gfx_widgets_start_achievement_notification();

   SLOCK_UNLOCK(cheevo_popup_queue_lock);
}
#endif

static void gfx_widgets_free(void)
{
   size_t i;
   gfx_animation_ctx_tag libretro_tag;

   widgets_inited = false;
   widgets_active = false;

   for (i = 0; i < widgets_len; i++)
   {
      const gfx_widget_t* widget = widgets[i];

      if (widget->free)
         widget->free();
   }

   /* Kill all running animations */
   gfx_animation_kill_by_tag(&gfx_widgets_generic_tag);

   /* Purge everything from the fifo */
   if (msg_queue)
   {
      while (fifo_read_avail(msg_queue) > 0)
      {
         menu_widget_msg_t *msg_widget;

         fifo_read(msg_queue, &msg_widget, sizeof(msg_widget));

         gfx_widgets_msg_queue_free(msg_widget, false);
         free(msg_widget);
      }

      fifo_free(msg_queue);
   }
   msg_queue = NULL;

   /* Purge everything from the list */
   if (current_msgs)
   {
      for (i = 0; i < current_msgs->size; i++)
      {
         menu_widget_msg_t *msg = (menu_widget_msg_t*)
            current_msgs->list[i].userdata;

         gfx_widgets_msg_queue_free(msg, false);
      }
      file_list_free(current_msgs);
   }
   current_msgs = NULL;

   msg_queue_tasks_count = 0;

#ifdef HAVE_CHEEVOS
   /* Achievement notification */
   if (cheevo_popup_queue_read_index >= 0)
   {
      SLOCK_LOCK(cheevo_popup_queue_lock);

      while (cheevo_popup_queue[cheevo_popup_queue_read_index].title)
         gfx_widgets_achievement_free_current();

      SLOCK_UNLOCK(cheevo_popup_queue_lock);
   }
#endif

   /* Font */
   video_coord_array_free(&font_raster_regular.carr);
   video_coord_array_free(&font_raster_bold.carr);

   font_driver_bind_block(NULL, NULL);

   /* Reset state of all other widgets */
   /* Libretro message */
   libretro_tag           = (uintptr_t) &libretro_message_timer;
   libretro_message_alpha = 0.0f;
   gfx_timer_kill(&libretro_message_timer);
   gfx_animation_kill_by_tag(&libretro_tag);

   /* AI Service overlay */
   /* ... */
}

bool gfx_widgets_set_fps_text(const char *new_fps_text)
{
   if (!widgets_active)
      return false;

   strlcpy(gfx_widgets_fps_text,
         new_fps_text, sizeof(gfx_widgets_fps_text));

   return true;
}

#ifdef HAVE_TRANSLATE
int gfx_widgets_ai_service_overlay_get_state(void)
{
   return ai_service_overlay_state;
}

bool gfx_widgets_ai_service_overlay_set_state(int state)
{
   ai_service_overlay_state = state;
   return true;
}

bool gfx_widgets_ai_service_overlay_load(
        char* buffer, unsigned buffer_len, enum image_type_enum image_type)
{
   if (ai_service_overlay_state == 0)
   {
      bool res = gfx_display_reset_textures_list_buffer(
               &ai_service_overlay_texture, 
               TEXTURE_FILTER_MIPMAP_LINEAR, 
               (void *) buffer, buffer_len, image_type,
               &ai_service_overlay_width, &ai_service_overlay_height);
      if (res)
         ai_service_overlay_state = 1;
      return res;
   }
   return true;
}

void gfx_widgets_ai_service_overlay_unload(void)
{
   if (ai_service_overlay_state == 1)
   {
      video_driver_texture_unload(&ai_service_overlay_texture);
      ai_service_overlay_texture = 0;
      ai_service_overlay_state   = 0;
   }
}
#endif

static void gfx_widgets_end_load_content_animation(void *userdata)
{
#if 0
   task_load_content_resume(); /* TODO: Restore that */
#endif
}

void gfx_widgets_cleanup_load_content_animation(void)
{
#ifdef HAVE_MENU
   load_content_animation_running = false;
   free(load_content_animation_content_name);
#endif
}

void gfx_widgets_start_load_content_animation(const char *content_name, bool remove_extension)
{
#ifdef HAVE_MENU
   /* TODO: finish the animation based on design, correct all timings */
   gfx_animation_ctx_entry_t entry;
   gfx_timer_ctx_entry_t timer_entry;
   int i;

   float icon_color[16] = COLOR_HEX_TO_FLOAT(0x0473C9, 1.0f); /* TODO: random color */
   unsigned timing      = 0;

   if (!widgets_active)
      return;

   /* Prepare data */
   load_content_animation_icon         = 0;

   /* Abort animation if we don't have an icon */
   if (!menu_driver_get_load_content_animation_data(&load_content_animation_icon,
      &load_content_animation_playlist_name) || !load_content_animation_icon)
   {
      gfx_widgets_end_load_content_animation(NULL);
      return;
   }

   load_content_animation_content_name = strdup(content_name);

   if (remove_extension)
      path_remove_extension(load_content_animation_content_name);

   /* Reset animation state */
   load_content_animation_icon_size          = load_content_animation_icon_size_initial;
   load_content_animation_icon_alpha         = 0.0f;
   load_content_animation_fade_alpha         = 0.0f;
   load_content_animation_final_fade_alpha   = 0.0f;

   memcpy(load_content_animation_icon_color, icon_color, sizeof(load_content_animation_icon_color));

   /* Setup the animation */
   entry.cb          = NULL;
   entry.easing_enum = EASING_OUT_QUAD;
   entry.tag         = gfx_widgets_generic_tag;
   entry.userdata    = NULL;

   /* Stage one: icon animation */
   /* Position */
   entry.duration       = ANIMATION_LOAD_CONTENT_DURATION;
   entry.subject        = &load_content_animation_icon_size;
   entry.target_value   = load_content_animation_icon_size_target;

   gfx_animation_push(&entry);

   /* Alpha */
   entry.subject        = &load_content_animation_icon_alpha;
   entry.target_value   = 1.0f;

   gfx_animation_push(&entry);
   timing += entry.duration;

   /* Stage two: backdrop + text */
   entry.duration       = ANIMATION_LOAD_CONTENT_DURATION*1.5;
   entry.subject        = &load_content_animation_fade_alpha;
   entry.target_value   = 1.0f;

   gfx_animation_push_delayed(timing, &entry);
   timing += entry.duration;

   /* Stage three: wait then color transition */
   timing += ANIMATION_LOAD_CONTENT_DURATION*1.5;

   entry.duration = ANIMATION_LOAD_CONTENT_DURATION*3;

   for (i = 0; i < 16; i++)
   {
      if (i == 3 || i == 7 || i == 11 || i == 15)
         continue;

      entry.subject        = &load_content_animation_icon_color[i];
      entry.target_value   = gfx_widgets_pure_white[i];

      gfx_animation_push_delayed(timing, &entry);
   }

   timing += entry.duration;

   /* Stage four: wait then make everything disappear */
   timing += ANIMATION_LOAD_CONTENT_DURATION*2;

   entry.duration       = ANIMATION_LOAD_CONTENT_DURATION*1.5;
   entry.subject        = &load_content_animation_final_fade_alpha;
   entry.target_value   = 1.0f;

   gfx_animation_push_delayed(timing, &entry);
   timing += entry.duration;

   /* Setup end */
   timer_entry.cb       = gfx_widgets_end_load_content_animation;
   timer_entry.duration = timing;
   timer_entry.userdata = NULL;

   gfx_timer_start(&load_content_animation_end_timer, &timer_entry);

   /* Draw all the things */
   load_content_animation_running = true;
#endif
}

#ifdef HAVE_CHEEVOS
static void gfx_widgets_achievement_dismiss(void *userdata)
{
   gfx_animation_ctx_entry_t entry;

   /* Slide up animation */
   entry.cb             = gfx_widgets_achievement_next;
   entry.duration       = MSG_QUEUE_ANIMATION_DURATION;
   entry.easing_enum    = EASING_OUT_QUAD;
   entry.subject        = &cheevo_y;
   entry.tag            = gfx_widgets_generic_tag;
   entry.target_value   = (float)(-(int)(cheevo_height));
   entry.userdata       = NULL;

   gfx_animation_push(&entry);
}

static void gfx_widgets_achievement_fold(void *userdata)
{
   gfx_animation_ctx_entry_t entry;

   /* Fold */
   entry.cb             = gfx_widgets_achievement_dismiss;
   entry.duration       = MSG_QUEUE_ANIMATION_DURATION;
   entry.easing_enum    = EASING_OUT_QUAD;
   entry.subject        = &cheevo_unfold;
   entry.tag            = gfx_widgets_generic_tag;
   entry.target_value   = 0.0f;
   entry.userdata       = NULL;

   gfx_animation_push(&entry);
}

static void gfx_widgets_achievement_unfold(void *userdata)
{
   gfx_animation_ctx_entry_t entry;
   gfx_timer_ctx_entry_t timer;

   /* Unfold */
   entry.cb             = NULL;
   entry.duration       = MSG_QUEUE_ANIMATION_DURATION;
   entry.easing_enum    = EASING_OUT_QUAD;
   entry.subject        = &cheevo_unfold;
   entry.tag            = gfx_widgets_generic_tag;
   entry.target_value   = 1.0f;
   entry.userdata       = NULL;

   gfx_animation_push(&entry);

   /* Wait before dismissing */
   timer.cb       = gfx_widgets_achievement_fold;
   timer.duration = MSG_QUEUE_ANIMATION_DURATION + CHEEVO_NOTIFICATION_DURATION;
   timer.userdata = NULL;

   gfx_timer_start(&cheevo_timer, &timer);
}

static void gfx_widgets_start_achievement_notification(void)
{
   gfx_animation_ctx_entry_t entry;
   cheevo_height        = widget_font_size * 4;
   cheevo_width         = MAX(
         font_driver_get_message_width(font_regular, msg_hash_to_str(MSG_ACHIEVEMENT_UNLOCKED), 0, 1),
         font_driver_get_message_width(font_regular, cheevo_popup_queue[cheevo_popup_queue_read_index].title, 0, 1)
   );
   cheevo_width        += simple_widget_padding * 2;
   cheevo_y             = (float)(-(int)cheevo_height);
   cheevo_unfold        = 0.0f;

   /* Slide down animation */
   entry.cb             = gfx_widgets_achievement_unfold;
   entry.duration       = MSG_QUEUE_ANIMATION_DURATION;
   entry.easing_enum    = EASING_OUT_QUAD;
   entry.subject        = &cheevo_y;
   entry.tag            = gfx_widgets_generic_tag;
   entry.target_value   = 0.0f;
   entry.userdata       = NULL;

   gfx_animation_push(&entry);
}

void gfx_widgets_push_achievement(const char *title, const char *badge)
{
   int start_notification = 1;

   if (!widgets_active)
      return;

   if (cheevo_popup_queue_read_index < 0)
   {
      /* queue uninitialized */
      memset(&cheevo_popup_queue, 0, sizeof(cheevo_popup_queue));
      cheevo_popup_queue_read_index = 0;

#ifdef HAVE_THREADS
      cheevo_popup_queue_lock = slock_new();
#endif
   }

   SLOCK_LOCK(cheevo_popup_queue_lock);

   if (cheevo_popup_queue_write_index == cheevo_popup_queue_read_index)
   {
      if (cheevo_popup_queue[cheevo_popup_queue_write_index].title)
      {
         /* queue full */
         SLOCK_UNLOCK(cheevo_popup_queue_lock);
         return;
      }

      /* queue empty */
   }
   else
   {
      /* notification already being displayed */
      start_notification = 0;
   }

   cheevo_popup_queue[cheevo_popup_queue_write_index].badge = cheevos_get_badge_texture(badge, 0);
   cheevo_popup_queue[cheevo_popup_queue_write_index].title = strdup(title);

   cheevo_popup_queue_write_index = (cheevo_popup_queue_write_index + 1) % CHEEVO_QUEUE_SIZE;

   if (start_notification)
      gfx_widgets_start_achievement_notification();

   SLOCK_UNLOCK(cheevo_popup_queue_lock);
}
#endif

static void gfx_widgets_libretro_message_fadeout(void *userdata)
{
   gfx_animation_ctx_entry_t entry;
   gfx_animation_ctx_tag tag = (uintptr_t) &libretro_message_timer;

   /* Start fade out animation */
   entry.cb             = NULL;
   entry.duration       = MSG_QUEUE_ANIMATION_DURATION;
   entry.easing_enum    = EASING_OUT_QUAD;
   entry.subject        = &libretro_message_alpha;
   entry.tag            = tag;
   entry.target_value   = 0.0f;
   entry.userdata       = NULL;

   gfx_animation_push(&entry);
}

void gfx_widgets_set_libretro_message(const char *msg, unsigned duration)
{
   gfx_timer_ctx_entry_t timer;
   gfx_animation_ctx_tag tag = (uintptr_t) &libretro_message_timer;

   if (!widgets_active)
      return;

   strlcpy(libretro_message, msg, sizeof(libretro_message));

   libretro_message_alpha = DEFAULT_BACKDROP;

   /* Kill and restart the timer / animation */
   gfx_timer_kill(&libretro_message_timer);
   gfx_animation_kill_by_tag(&tag);

   timer.cb       = gfx_widgets_libretro_message_fadeout;
   timer.duration = duration;
   timer.userdata = NULL;

   gfx_timer_start(&libretro_message_timer, &timer);

   /* Compute text width */
   libretro_message_width = font_driver_get_message_width(font_regular, msg, (unsigned)strlen(msg), 1) + simple_widget_padding * 2;
}
