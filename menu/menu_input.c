/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
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

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include <string/stdstring.h>

#include "widgets/menu_input_dialog.h"
#include "widgets/menu_input_bind_dialog.h"
#include "widgets/menu_osk.h"

#include "menu_driver.h"
#include "menu_input.h"
#include "menu_animation.h"

#include "../configuration.h"
#include "../retroarch.h"
#include "../performance_counters.h"
#include "../tasks/tasks_internal.h"

enum menu_mouse_action
{
   MENU_MOUSE_ACTION_NONE = 0,
   MENU_MOUSE_ACTION_BUTTON_L,
   MENU_MOUSE_ACTION_BUTTON_L_TOGGLE,
   MENU_MOUSE_ACTION_BUTTON_L_SET_NAVIGATION,
   MENU_MOUSE_ACTION_BUTTON_R,
   MENU_MOUSE_ACTION_WHEEL_UP,
   MENU_MOUSE_ACTION_WHEEL_DOWN,
   MENU_MOUSE_ACTION_HORIZ_WHEEL_UP,
   MENU_MOUSE_ACTION_HORIZ_WHEEL_DOWN
};

static unsigned char menu_keyboard_key_state[RETROK_LAST] = {0};

static unsigned mouse_old_x               = 0;
static unsigned mouse_old_y               = 0;
static menu_input_t menu_input_state;

static rarch_timer_t mouse_activity_timer = {0};

/* This function gets called for handling pointer events.
 *
 * Pointer events are touchscreen events that are spawned
 * by touchpad/touchscreen. */
static int menu_event_pointer(unsigned *action)
{
   rarch_joypad_info_t joypad_info;
   int pointer_x, pointer_y;
   size_t fb_pitch;
   unsigned fb_width, fb_height;
   const struct retro_keybind *binds[MAX_USERS] = {NULL};
   const input_driver_t *input_ptr              = input_get_ptr();
   void *input_data                             = input_get_data();
   menu_input_t *menu_input                     = &menu_input_state;
   int pointer_device                           = menu_driver_is_texture_set()
      ?
      RETRO_DEVICE_POINTER : RARCH_DEVICE_POINTER_SCREEN;

   menu_display_get_fb_size(&fb_width, &fb_height,
         &fb_pitch);

   joypad_info.joy_idx                          = 0;
   joypad_info.auto_binds                       = NULL;
   joypad_info.axis_threshold                   = 0.0f;

   pointer_x                                    =
      input_ptr->input_state(input_data, joypad_info, binds,
            0, pointer_device, 0, RETRO_DEVICE_ID_POINTER_X);
   pointer_y                                    =
      input_ptr->input_state(input_data, joypad_info, binds,
            0, pointer_device, 0, RETRO_DEVICE_ID_POINTER_Y);

   menu_input->pointer.pressed[0]  = input_ptr->input_state(input_data,
         joypad_info,
         binds,
         0, pointer_device, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
   menu_input->pointer.pressed[1]  = input_ptr->input_state(input_data,
         joypad_info,
         binds,
         0, pointer_device, 1, RETRO_DEVICE_ID_POINTER_PRESSED);
   menu_input->pointer.back        = input_ptr->input_state(input_data,
         joypad_info,
         binds,
         0, pointer_device, 0, RARCH_DEVICE_ID_POINTER_BACK);

   menu_input->pointer.x = ((pointer_x + 0x7fff) * (int)fb_width) / 0xFFFF;
   menu_input->pointer.y = ((pointer_y + 0x7fff) * (int)fb_height) / 0xFFFF;

   return 0;
}

/* Check if a specific keyboard key has been pressed. */
unsigned char menu_event_kb_is_set(enum retro_key key)
{
   return menu_keyboard_key_state[key];
}

/* Set a specific keyboard key latch. */
static void menu_event_kb_set_internal(unsigned idx, unsigned char key)
{
   menu_keyboard_key_state[idx] = key;
}

/* Set a specific keyboard key.
 *
 * 'down' sets the latch (true would
 * mean the key is being pressed down, while 'false' would mean that
 * the key has been released).
 **/
void menu_event_kb_set(bool down, enum retro_key key)
{
   if (key == RETROK_UNKNOWN)
   {
      unsigned i;

      for (i = 0; i < RETROK_LAST; i++)
         menu_event_kb_set_internal(i, (menu_event_kb_is_set((enum retro_key)i) & 1) << 1);
   }
   else
      menu_event_kb_set_internal(key, ((menu_event_kb_is_set(key) & 1) << 1) | down);
}

/*
 * This function gets called in order to process all input events
 * for the current frame.
 *
 * Sends input code to menu for one frame.
 *
 * It uses as input the local variables' input' and 'trigger_input'.
 *
 * Mouse and touch input events get processed inside this function.
 *
 * NOTE: 'input' and 'trigger_input' is sourced from the keyboard and/or
 * the gamepad. It does not contain input state derived from the mouse
 * and/or touch - this gets dealt with separately within this function.
 *
 * TODO/FIXME - maybe needs to be overhauled so we can send multiple
 * events per frame if we want to, and we shouldn't send the
 * entire button state either but do a separate event per button
 * state.
 */
unsigned menu_event(input_bits_t *p_input, input_bits_t *p_trigger_input)
{
   /* Used for key repeat */
   static float delay_timer                = 0.0f;
   static float delay_count                = 0.0f;
   static unsigned ok_old                  = 0;
   unsigned ret                            = MENU_ACTION_NOOP;
   static bool initial_held                = true;
   static bool first_held                  = false;
   bool set_scroll                         = false;
   bool mouse_enabled                      = false;
   size_t new_scroll_accel                 = 0;
   menu_input_t *menu_input                = NULL;
   settings_t *settings                    = config_get_ptr();
   bool swap_ok_cancel_btns                = settings->bools.input_menu_swap_ok_cancel_buttons;
   bool input_swap_override                =
      input_autoconfigure_get_swap_override();
   unsigned menu_ok_btn                    = (!input_swap_override &&
      swap_ok_cancel_btns) ?
      RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_A;
   unsigned menu_cancel_btn                = (!input_swap_override &&
      swap_ok_cancel_btns) ?
      RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_B;
   unsigned ok_current                     = BIT256_GET_PTR(p_input,
         menu_ok_btn );
   unsigned ok_trigger                     = ok_current & ~ok_old;

   ok_old                                  = ok_current;

   if (bits_any_set(p_input->data, ARRAY_SIZE(p_input->data)))
   {
      if (!first_held)
      {
         /* don't run anything first frame, only capture held inputs
          * for old_input_state. */

         first_held  = true;
         delay_timer = initial_held ? 300 : 150;
         delay_count = 0;
      }

      if (delay_count >= delay_timer)
      {
         uint32_t input_repeat = 0;
         BIT32_SET(input_repeat, RETRO_DEVICE_ID_JOYPAD_UP);
         BIT32_SET(input_repeat, RETRO_DEVICE_ID_JOYPAD_DOWN);
         BIT32_SET(input_repeat, RETRO_DEVICE_ID_JOYPAD_LEFT);
         BIT32_SET(input_repeat, RETRO_DEVICE_ID_JOYPAD_RIGHT);
         BIT32_SET(input_repeat, RETRO_DEVICE_ID_JOYPAD_L);
         BIT32_SET(input_repeat, RETRO_DEVICE_ID_JOYPAD_R);

         set_scroll           = true;
         first_held           = false;
         p_trigger_input->data[0] |= p_input->data[0] & input_repeat;

         menu_driver_ctl(MENU_NAVIGATION_CTL_GET_SCROLL_ACCEL,
               &new_scroll_accel);

         new_scroll_accel = MIN(new_scroll_accel + 1, 64);
      }

      initial_held  = false;
   }
   else
   {
      set_scroll   = true;
      first_held   = false;
      initial_held = true;
   }

   if (set_scroll)
      menu_driver_ctl(MENU_NAVIGATION_CTL_SET_SCROLL_ACCEL,
            &new_scroll_accel);

   delay_count += menu_animation_get_delta_time();

   if (menu_input_dialog_get_display_kb())
   {
      menu_event_osk_iterate();

      if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_DOWN))
      {
         if (menu_event_get_osk_ptr() < 33)
            menu_event_set_osk_ptr(menu_event_get_osk_ptr()
                  + OSK_CHARS_PER_LINE);
      }

      if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_UP))
      {
         if (menu_event_get_osk_ptr() >= OSK_CHARS_PER_LINE)
            menu_event_set_osk_ptr(menu_event_get_osk_ptr()
                  - OSK_CHARS_PER_LINE);
      }

      if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_RIGHT))
      {
         if (menu_event_get_osk_ptr() < 43)
            menu_event_set_osk_ptr(menu_event_get_osk_ptr() + 1);
      }

      if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_LEFT))
      {
         if (menu_event_get_osk_ptr() >= 1)
            menu_event_set_osk_ptr(menu_event_get_osk_ptr() - 1);
      }

      if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_L))
      {
         if (menu_event_get_osk_idx() > OSK_TYPE_UNKNOWN + 1)
            menu_event_set_osk_idx((enum osk_type)(
                     menu_event_get_osk_idx() - 1));
         else
            menu_event_set_osk_idx((enum osk_type)(OSK_TYPE_LAST - 1));
      }

      if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_R))
      {
         if (menu_event_get_osk_idx() < OSK_TYPE_LAST - 1)
            menu_event_set_osk_idx((enum osk_type)(
                     menu_event_get_osk_idx() + 1));
         else
            menu_event_set_osk_idx((enum osk_type)(OSK_TYPE_UNKNOWN + 1));
      }

      if (BIT256_GET_PTR(p_trigger_input, menu_ok_btn))
      {
         if (menu_event_get_osk_ptr() >= 0)
            menu_event_osk_append(menu_event_get_osk_ptr());
      }

      if (BIT256_GET_PTR(p_trigger_input, menu_cancel_btn))
         input_keyboard_event(true, '\x7f', '\x7f',
               0, RETRO_DEVICE_KEYBOARD);

      /* send return key to close keyboard input window */
      if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_START))
         input_keyboard_event(true, '\n', '\n', 0, RETRO_DEVICE_KEYBOARD);

      BIT256_CLEAR_ALL_PTR(p_trigger_input);
   }
   else
   {
      if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_UP))
         ret = MENU_ACTION_UP;
      else if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_DOWN))
         ret = MENU_ACTION_DOWN;
      else if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_LEFT))
         ret = MENU_ACTION_LEFT;
      else if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_RIGHT))
         ret = MENU_ACTION_RIGHT;
      else if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_L))
         ret = MENU_ACTION_SCROLL_UP;
      else if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_R))
         ret = MENU_ACTION_SCROLL_DOWN;
      else if (ok_trigger)
         ret = MENU_ACTION_OK;
      else if (BIT256_GET_PTR(p_trigger_input, menu_cancel_btn))
         ret = MENU_ACTION_CANCEL;
      else if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_X))
         ret = MENU_ACTION_SEARCH;
      else if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_Y))
         ret = MENU_ACTION_SCAN;
      else if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_START))
         ret = MENU_ACTION_START;
      else if (BIT256_GET_PTR(p_trigger_input, RETRO_DEVICE_ID_JOYPAD_SELECT))
         ret = MENU_ACTION_INFO;
      else if (BIT256_GET_PTR(p_trigger_input, RARCH_MENU_TOGGLE))
         ret = MENU_ACTION_TOGGLE;
   }

   if (menu_event_kb_is_set(RETROK_F11))
   {
      command_event(CMD_EVENT_GRAB_MOUSE_TOGGLE, NULL);
      menu_event_kb_set_internal(RETROK_F11, 0);
   }

   mouse_enabled                      = settings->bools.menu_mouse_enable;
#ifdef HAVE_OVERLAY
   if (!mouse_enabled)
      mouse_enabled = !(settings->bools.input_overlay_enable
            && input_overlay_is_alive(overlay_ptr));
#endif

   menu_input = &menu_input_state;

   if (!mouse_enabled)
      menu_input->mouse.ptr = 0;

   if (settings->bools.menu_pointer_enable)
      menu_event_pointer(&ret);
   else
   {
      menu_input->pointer.x          = 0;
      menu_input->pointer.y          = 0;
      menu_input->pointer.dx         = 0;
      menu_input->pointer.dy         = 0;
      menu_input->pointer.accel      = 0;
      menu_input->pointer.pressed[0] = false;
      menu_input->pointer.pressed[1] = false;
      menu_input->pointer.back       = false;
      menu_input->pointer.ptr        = 0;
   }

   return ret;
}

bool menu_input_mouse_check_vector_inside_hitbox(menu_input_ctx_hitbox_t *hitbox)
{
   int16_t  mouse_x       = menu_input_mouse_state(MENU_MOUSE_X_AXIS);
   int16_t  mouse_y       = menu_input_mouse_state(MENU_MOUSE_Y_AXIS);
   bool     inside_hitbox =
      (mouse_x    >= hitbox->x1)
      && (mouse_x <= hitbox->x2)
      && (mouse_y >= hitbox->y1)
      && (mouse_y <= hitbox->y2)
      ;

   return inside_hitbox;
}

bool menu_input_ctl(enum menu_input_ctl_state state, void *data)
{
   static bool pointer_dragging                 = false;
   menu_input_t *menu_input                     = &menu_input_state;

   if (!menu_input)
      return false;

   switch (state)
   {
      case MENU_INPUT_CTL_DEINIT:
         memset(menu_input, 0, sizeof(menu_input_t));
         pointer_dragging      = false;
         break;
      case MENU_INPUT_CTL_MOUSE_PTR:
         menu_input->mouse.ptr = (*(unsigned*)data);
         break;
      case MENU_INPUT_CTL_POINTER_PTR:
         menu_input->pointer.ptr = (*(unsigned*)data);
         break;
      case MENU_INPUT_CTL_POINTER_ACCEL_READ:
         {
            float *ptr = (float*)data;
            *ptr = menu_input->pointer.accel;
         }
         break;
      case MENU_INPUT_CTL_POINTER_ACCEL_WRITE:
         menu_input->pointer.accel = (*(float*)data);
         break;
      case MENU_INPUT_CTL_IS_POINTER_DRAGGED:
         return pointer_dragging;
      case MENU_INPUT_CTL_SET_POINTER_DRAGGED:
         pointer_dragging = true;
         break;
      case MENU_INPUT_CTL_UNSET_POINTER_DRAGGED:
         pointer_dragging = false;
         break;
      case MENU_INPUT_CTL_NONE:
         break;
   }

   return true;
}

static int menu_input_mouse_post_iterate(uint64_t *input_mouse,
      menu_file_list_cbs_t *cbs, unsigned action, bool *mouse_activity)
{
   settings_t *settings       = config_get_ptr();
   static bool mouse_oldleft  = false;
   static bool mouse_oldright = false;

   if (
         !settings->bools.menu_mouse_enable
#ifdef HAVE_OVERLAY
         || (settings->bools.input_overlay_enable && input_overlay_is_alive(overlay_ptr))
#endif
         )
   {
      /* HACK: Need to lie to avoid false hits if mouse is held
       * when entering the RetroArch window. */

      /* This happens if, for example, someone double clicks the
       * window border to maximize it.
       *
       * The proper fix is, of course, triggering on WM_LBUTTONDOWN
       * rather than this state change. */
      mouse_oldleft   = true;
      mouse_oldright  = true;
      return 0;
   }

   if (menu_input_mouse_state(MENU_MOUSE_LEFT_BUTTON))
   {
      if (!mouse_oldleft)
      {
         menu_input_t *menu_input = &menu_input_state;
         size_t selection         = menu_navigation_get_selection();

         BIT64_SET(*input_mouse, MENU_MOUSE_ACTION_BUTTON_L);

         mouse_oldleft = true;

         if ((menu_input->mouse.ptr == selection) && cbs && cbs->action_select)
         {
            BIT64_SET(*input_mouse, MENU_MOUSE_ACTION_BUTTON_L_TOGGLE);
         }
         else if (menu_input->mouse.ptr <= (menu_entries_get_size() - 1))
         {
            BIT64_SET(*input_mouse, MENU_MOUSE_ACTION_BUTTON_L_SET_NAVIGATION);
         }

         *mouse_activity = true;
      }
   }
   else
      mouse_oldleft = false;

   if (menu_input_mouse_state(MENU_MOUSE_RIGHT_BUTTON))
   {
      if (!mouse_oldright)
      {
         mouse_oldright = true;
         BIT64_SET(*input_mouse, MENU_MOUSE_ACTION_BUTTON_R);
         *mouse_activity = true;
      }
   }
   else
      mouse_oldright = false;

   if (menu_input_mouse_state(MENU_MOUSE_WHEEL_DOWN))
   {
      BIT64_SET(*input_mouse, MENU_MOUSE_ACTION_WHEEL_DOWN);
      *mouse_activity = true;
   }

   if (menu_input_mouse_state(MENU_MOUSE_WHEEL_UP))
   {
      BIT64_SET(*input_mouse, MENU_MOUSE_ACTION_WHEEL_UP);
      *mouse_activity = true;
   }

   if (menu_input_mouse_state(MENU_MOUSE_HORIZ_WHEEL_DOWN))
   {
      BIT64_SET(*input_mouse, MENU_MOUSE_ACTION_HORIZ_WHEEL_DOWN);
      *mouse_activity = true;
   }

   if (menu_input_mouse_state(MENU_MOUSE_HORIZ_WHEEL_UP))
   {
      BIT64_SET(*input_mouse, MENU_MOUSE_ACTION_HORIZ_WHEEL_UP);
      *mouse_activity = true;
   }

   return 0;
}

static int menu_input_mouse_frame(
      menu_file_list_cbs_t *cbs, menu_entry_t *entry,
      unsigned action)
{
   bool mouse_activity      = false;
   bool no_mouse_activity   = false;
   uint64_t mouse_state     = MENU_MOUSE_ACTION_NONE;
   int ret                  = 0;
   settings_t *settings     = config_get_ptr();
   menu_input_t *menu_input = &menu_input_state;
   bool mouse_enable        = settings->bools.menu_mouse_enable;

   if (mouse_enable)
      ret  = menu_input_mouse_post_iterate(&mouse_state, cbs, action, &mouse_activity);

   if ((settings->bools.menu_pointer_enable || mouse_enable))
   {
      menu_ctx_pointer_t point;
      point.x       = menu_input_mouse_state(MENU_MOUSE_X_AXIS);
      point.y       = menu_input_mouse_state(MENU_MOUSE_Y_AXIS);
      point.ptr     = 0;
      point.cbs     = NULL;
      point.entry   = NULL;
      point.action  = 0;
      point.retcode = 0;

      if (menu_input_dialog_get_display_kb())
         menu_driver_ctl(RARCH_MENU_CTL_OSK_PTR_AT_POS, &point);

      if (rarch_timer_is_running(&mouse_activity_timer))
         rarch_timer_tick(&mouse_activity_timer);

      if (mouse_old_x != point.x || mouse_old_y != point.y)
      {
         if (!rarch_timer_is_running(&mouse_activity_timer))
            mouse_activity = true;
         menu_event_set_osk_ptr(point.retcode);
      }
      else
      {
         if (rarch_timer_has_expired(&mouse_activity_timer))
            no_mouse_activity = true;
      }
      mouse_old_x = point.x;
      mouse_old_y = point.y;
   }

   if (BIT64_GET(mouse_state, MENU_MOUSE_ACTION_BUTTON_L))
   {
      menu_ctx_pointer_t point;

      point.x      = menu_input_mouse_state(MENU_MOUSE_X_AXIS);
      point.y      = menu_input_mouse_state(MENU_MOUSE_Y_AXIS);
      point.ptr    = menu_input->mouse.ptr;
      point.cbs    = cbs;
      point.entry  = entry;
      point.action = action;

      if (menu_input_dialog_get_display_kb())
      {
         menu_driver_ctl(RARCH_MENU_CTL_OSK_PTR_AT_POS, &point);
         if (point.retcode > -1)
         {
            menu_event_set_osk_ptr(point.retcode);
            menu_event_osk_append(point.retcode);
         }
      }
      else
      {
         menu_driver_ctl(RARCH_MENU_CTL_POINTER_UP, &point);
         menu_driver_ctl(RARCH_MENU_CTL_POINTER_TAP, &point);
         ret = point.retcode;
      }
   }

   if (BIT64_GET(mouse_state, MENU_MOUSE_ACTION_BUTTON_R))
   {
      size_t selection = menu_navigation_get_selection();
      menu_entry_action(entry, (unsigned)selection, MENU_ACTION_CANCEL);
   }

   if (BIT64_GET(mouse_state, MENU_MOUSE_ACTION_WHEEL_DOWN))
   {
      unsigned increment_by = 1;
      menu_driver_ctl(MENU_NAVIGATION_CTL_INCREMENT, &increment_by);
   }

   if (BIT64_GET(mouse_state, MENU_MOUSE_ACTION_WHEEL_UP))
   {
      unsigned decrement_by = 1;
      menu_driver_ctl(MENU_NAVIGATION_CTL_DECREMENT, &decrement_by);
   }

   if (BIT64_GET(mouse_state, MENU_MOUSE_ACTION_HORIZ_WHEEL_UP))
   {
      /* stub */
   }

   if (BIT64_GET(mouse_state, MENU_MOUSE_ACTION_HORIZ_WHEEL_DOWN))
   {
      /* stub */
   }

   if (mouse_activity)
   {
      menu_ctx_environment_t menu_environ;

      rarch_timer_begin(&mouse_activity_timer, 4);
      menu_environ.type = MENU_ENVIRON_ENABLE_MOUSE_CURSOR;
      menu_environ.data = NULL;

      menu_driver_ctl(RARCH_MENU_CTL_ENVIRONMENT, &menu_environ);
   }

   if (no_mouse_activity)
   {
      menu_ctx_environment_t menu_environ;

      rarch_timer_end(&mouse_activity_timer);
      menu_environ.type = MENU_ENVIRON_DISABLE_MOUSE_CURSOR;
      menu_environ.data = NULL;

      menu_driver_ctl(RARCH_MENU_CTL_ENVIRONMENT, &menu_environ);
   }

   return ret;
}

int16_t menu_input_pointer_state(enum menu_input_pointer_state state)
{
   menu_input_t *menu_input = &menu_input_state;

   if (!menu_input)
      return 0;

   switch (state)
   {
      case MENU_POINTER_X_AXIS:
         return menu_input->pointer.x;
      case MENU_POINTER_Y_AXIS:
         return menu_input->pointer.y;
      case MENU_POINTER_DELTA_X_AXIS:
         return menu_input->pointer.dx;
      case MENU_POINTER_DELTA_Y_AXIS:
         return menu_input->pointer.dy;
      case MENU_POINTER_PRESSED:
         return menu_input->pointer.pressed[0];
   }

   return 0;
}

int16_t menu_input_mouse_state(enum menu_input_mouse_state state)
{
   rarch_joypad_info_t joypad_info;
   const input_driver_t *input_ptr = input_get_ptr();
   void *input_data                = input_get_data();
   unsigned type                   = 0;
   unsigned device                 = RETRO_DEVICE_MOUSE;

   joypad_info.joy_idx             = 0;
   joypad_info.auto_binds          = NULL;
   joypad_info.axis_threshold      = 0.0f;

   switch (state)
   {
      case MENU_MOUSE_X_AXIS:
         device = RARCH_DEVICE_MOUSE_SCREEN;
         type = RETRO_DEVICE_ID_MOUSE_X;
         break;
      case MENU_MOUSE_Y_AXIS:
         device = RARCH_DEVICE_MOUSE_SCREEN;
         type = RETRO_DEVICE_ID_MOUSE_Y;
         break;
      case MENU_MOUSE_LEFT_BUTTON:
         type = RETRO_DEVICE_ID_MOUSE_LEFT;
         break;
      case MENU_MOUSE_RIGHT_BUTTON:
         type = RETRO_DEVICE_ID_MOUSE_RIGHT;
         break;
      case MENU_MOUSE_WHEEL_UP:
         type = RETRO_DEVICE_ID_MOUSE_WHEELUP;
         break;
      case MENU_MOUSE_WHEEL_DOWN:
         type = RETRO_DEVICE_ID_MOUSE_WHEELDOWN;
         break;
      case MENU_MOUSE_HORIZ_WHEEL_UP:
         type = RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP;
         break;
      case MENU_MOUSE_HORIZ_WHEEL_DOWN:
         type = RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN;
         break;
   }

   return input_ptr->input_state(input_data, joypad_info,
         NULL, 0, device, 0, type);
}

static int menu_input_pointer_post_iterate(
      menu_file_list_cbs_t *cbs,
      menu_entry_t *entry, unsigned action)
{
   static bool pointer_oldpressed[2];
   static bool pointer_oldback  = false;
   static int16_t start_x       = 0;
   static int16_t start_y       = 0;
   static int16_t pointer_old_x = 0;
   static int16_t pointer_old_y = 0;
   int ret                      = 0;
   menu_input_t *menu_input     = &menu_input_state;
   settings_t *settings         = config_get_ptr();

   if (!menu_input || !settings)
      return -1;

#ifdef HAVE_OVERLAY
   /* If we have overlays enabled, overlay controls take
    * precedence and we don't want regular menu
    * pointer controls to be handled */
   if ((       settings->bools.input_overlay_enable
            && input_overlay_is_alive(overlay_ptr)))
      return 0;
#endif

   if (menu_input->pointer.pressed[0])
   {
      gfx_ctx_metrics_t metrics;
      float dpi;
      static float accel0       = 0.0f;
      static float accel1       = 0.0f;
      int16_t pointer_x         = menu_input_pointer_state(MENU_POINTER_X_AXIS);
      int16_t pointer_y         = menu_input_pointer_state(MENU_POINTER_Y_AXIS);

      metrics.type  = DISPLAY_METRIC_DPI;
      metrics.value = &dpi;

      menu_input->pointer.counter++;

      if (menu_input->pointer.counter == 1 &&
            !menu_input_ctl(MENU_INPUT_CTL_IS_POINTER_DRAGGED, NULL))
      {
         menu_ctx_pointer_t point;

         point.x                           = pointer_x;
         point.y                           = pointer_y;
         point.ptr                         = menu_input->pointer.ptr;
         point.cbs                         = cbs;
         point.entry                       = entry;
         point.action                      = action;

         menu_driver_ctl(RARCH_MENU_CTL_POINTER_DOWN, &point);
      }

      if (!pointer_oldpressed[0])
      {
         menu_input->pointer.accel         = 0;
         accel0                            = 0;
         accel1                            = 0;
         start_x                           = pointer_x;
         start_y                           = pointer_y;
         pointer_old_x                     = pointer_x;
         pointer_old_y                     = pointer_y;
         pointer_oldpressed[0]             = true;
      }
      else if (video_context_driver_get_metrics(&metrics))
      {
         if (abs(pointer_x - start_x) > (dpi / 10)
               || abs(pointer_y - start_y) > (dpi / 10))
         {
            float s;

            menu_input_ctl(MENU_INPUT_CTL_SET_POINTER_DRAGGED, NULL);
            menu_input->pointer.dx            = pointer_x - pointer_old_x;
            menu_input->pointer.dy            = pointer_y - pointer_old_y;
            pointer_old_x                     = pointer_x;
            pointer_old_y                     = pointer_y;

            s = menu_input->pointer.dy;
            menu_input->pointer.accel = (accel0 + accel1 + s) / 3;
            accel0                    = accel1;
            accel1                    = menu_input->pointer.accel;
         }
      }
   }
   else
   {
      if (pointer_oldpressed[0])
      {
         if (!menu_input_ctl(MENU_INPUT_CTL_IS_POINTER_DRAGGED, NULL))
         {
            menu_ctx_pointer_t point;

            point.x      = start_x;
            point.y      = start_y;
            point.ptr    = menu_input->pointer.ptr;
            point.cbs    = cbs;
            point.entry  = entry;
            point.action = action;

            if (menu_input_dialog_get_display_kb())
            {
               menu_driver_ctl(RARCH_MENU_CTL_OSK_PTR_AT_POS, &point);
               if (point.retcode > -1)
               {
                  menu_event_set_osk_ptr(point.retcode);
                  menu_event_osk_append(point.retcode);
               }
            }
            else
            {
               if (menu_input->pointer.counter > 32)
               {
                  size_t selection = menu_navigation_get_selection();
                  if (cbs && cbs->action_start)
                     return menu_entry_action(entry, (unsigned)selection, MENU_ACTION_START);

               }
               else
               {
                  menu_driver_ctl(RARCH_MENU_CTL_POINTER_UP, &point);
                  menu_driver_ctl(RARCH_MENU_CTL_POINTER_TAP, &point);
                  ret = point.retcode;
               }
            }
         }

         pointer_oldpressed[0]             = false;
         start_x                           = 0;
         start_y                           = 0;
         pointer_old_x                     = 0;
         pointer_old_y                     = 0;
         menu_input->pointer.dx            = 0;
         menu_input->pointer.dy            = 0;
         menu_input->pointer.counter       = 0;

         menu_input_ctl(MENU_INPUT_CTL_UNSET_POINTER_DRAGGED, NULL);
      }
   }

   if (menu_input->pointer.back)
   {
      if (!pointer_oldback)
      {
         pointer_oldback = true;
         menu_entry_action(entry, (unsigned)menu_navigation_get_selection(), MENU_ACTION_CANCEL);
      }
   }

   pointer_oldback = menu_input->pointer.back;

   return ret;
}

void menu_input_post_iterate(int *ret, unsigned action)
{
   menu_entry_t entry;
   settings_t *settings       = config_get_ptr();
   file_list_t *selection_buf = menu_entries_get_selection_buf_ptr(0);
   size_t selection           = menu_navigation_get_selection();
   menu_file_list_cbs_t *cbs  = selection_buf ?
      (menu_file_list_cbs_t*)file_list_get_actiondata_at_offset(selection_buf, selection) : NULL;

   menu_entry_init(&entry);
   menu_entry_get(&entry, 0, selection, NULL, false);

   *ret = menu_input_mouse_frame(cbs, &entry, action);

   if (settings->bools.menu_pointer_enable)
      *ret |= menu_input_pointer_post_iterate(cbs, &entry, action);

   menu_entry_free(&entry);
}
