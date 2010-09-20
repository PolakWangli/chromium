// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GTK_MENU_GTK_H_
#define CHROME_BROWSER_GTK_MENU_GTK_H_
#pragma once

#include <gtk/gtk.h>

#include <string>
#include <vector>

#include "app/gtk_signal.h"
#include "base/task.h"
#include "gfx/point.h"

class SkBitmap;

namespace menus {
class ButtonMenuItemModel;
class MenuModel;
}

class MenuGtk {
 public:
  // Delegate class that lets another class control the status of the menu.
  class Delegate {
   public:
    virtual ~Delegate() { }

    // Called before a command is executed. This exists for the case where a
    // model is handling the actual execution of commands, but the delegate
    // still needs to know that some command got executed. This is called before
    // and not after the command is executed because its execution may delete
    // the menu and/or the delegate.
    virtual void CommandWillBeExecuted() {}

    // Called when the menu stops showing. This will be called before
    // ExecuteCommand if the user clicks an item, but will also be called when
    // the user clicks away from the menu.
    virtual void StoppedShowing() {}

    // Return true if we should override the "gtk-menu-images" system setting
    // when showing image menu items for this menu.
    virtual bool AlwaysShowImages() const { return false; }

    // Returns a tinted image used in button in a menu.
    virtual GtkIconSet* GetIconSetForId(int idr) { return NULL; }
  };

  MenuGtk(MenuGtk::Delegate* delegate, menus::MenuModel* model);
  ~MenuGtk();

  // Initialize GTK signal handlers.
  void ConnectSignalHandlers();

  // These methods are used to build the menu dynamically. The return value
  // is the new menu item.
  GtkWidget* AppendMenuItemWithLabel(int command_id, const std::string& label);
  GtkWidget* AppendMenuItemWithIcon(int command_id, const std::string& label,
                                    const SkBitmap& icon);
  GtkWidget* AppendCheckMenuItemWithLabel(int command_id,
                                          const std::string& label);
  GtkWidget* AppendSeparator();
  GtkWidget* AppendMenuItem(int command_id, GtkWidget* menu_item);
  GtkWidget* AppendMenuItemToMenu(int index,
                                  menus::MenuModel* model,
                                  GtkWidget* menu_item,
                                  GtkWidget* menu,
                                  bool connect_to_activate);

  // Displays the menu. |timestamp| is the time of activation. The popup is
  // statically positioned at |widget|.
  void Popup(GtkWidget* widget, gint button_type, guint32 timestamp);

  // Displays the menu using the button type and timestamp of |event|. The popup
  // is statically positioned at |widget|.
  void Popup(GtkWidget* widget, GdkEvent* event);

  // Displays the menu as a context menu, i.e. at the current cursor location.
  // |event_time| is the time of the event that triggered the menu's display.
  void PopupAsContext(guint32 event_time);

  // Displays the menu at the given coords. |point| is intentionally not const.
  void PopupAsContextAt(guint32 event_time, gfx::Point point);

  // Displays the menu as a context menu for the passed status icon.
  void PopupAsContextForStatusIcon(guint32 event_time, guint32 button,
                                   GtkStatusIcon* icon);

  // Displays the menu following a keyboard event (such as selecting |widget|
  // and pressing "enter").
  void PopupAsFromKeyEvent(GtkWidget* widget);

  // Closes the menu.
  void Cancel();

  // Repositions the menu to be right under the button.  Alignment is set as
  // object data on |void_widget| with the tag "left_align".  If "left_align"
  // is true, it aligns the left side of the menu with the left side of the
  // button. Otherwise it aligns the right side of the menu with the right side
  // of the button. Public since some menus have odd requirements that don't
  // belong in a public class.
  static void WidgetMenuPositionFunc(GtkMenu* menu,
                                     int* x,
                                     int* y,
                                     gboolean* push_in,
                                     void* void_widget);

  // Positions the menu to appear at the gfx::Point represented by |userdata|.
  static void PointMenuPositionFunc(GtkMenu* menu,
                                    int* x,
                                    int* y,
                                    gboolean* push_in,
                                    gpointer userdata);

  GtkWidget* widget() const { return menu_; }

  // Updates all the enabled/checked states and the dynamic labels.
  void UpdateMenu();

 private:
  // Builds a GtkImageMenuItem.
  GtkWidget* BuildMenuItemWithImage(const std::string& label,
                                    const SkBitmap& icon);

  // A function that creates a GtkMenu from |model_|.
  void BuildMenuFromModel();
  // Implementation of the above; called recursively.
  void BuildSubmenuFromModel(menus::MenuModel* model, GtkWidget* menu);
  // Builds a menu item with buttons in it from the data in the model.
  GtkWidget* BuildButtomMenuItem(menus::ButtonMenuItemModel* model,
                                 GtkWidget* menu);

  void ExecuteCommand(menus::MenuModel* model, int id);

  // Callback for when a menu item is clicked.
  CHROMEGTK_CALLBACK_0(MenuGtk, void, OnMenuItemActivated);

  // Called when one of the buttons are pressed.
  CHROMEGTK_CALLBACK_1(MenuGtk, void, OnMenuButtonPressed, int);

  // Called to maybe activate a button if that button isn't supposed to dismiss
  // the menu.
  CHROMEGTK_CALLBACK_1(MenuGtk, gboolean, OnMenuTryButtonPressed, int);

  // Updates all the menu items' state.
  CHROMEGTK_CALLBACK_0(MenuGtk, void, OnMenuShow);

  // Sets the activating widget back to a normal appearance.
  CHROMEGTK_CALLBACK_0(MenuGtk, void, OnMenuHidden);

  // Sets the enable/disabled state and dynamic labels on our menu items.
  static void SetButtonItemInfo(GtkWidget* button, gpointer userdata);

  // Sets the check mark, enabled/disabled state and dynamic labels on our menu
  // items.
  static void SetMenuItemInfo(GtkWidget* widget, void* raw_menu);

  // Queries this object about the menu state.
  MenuGtk::Delegate* delegate_;

  // If non-NULL, the MenuModel that we use to populate and control the GTK
  // menu (overriding the delegate as a controller).
  menus::MenuModel* model_;

  // For some menu items, we want to show the accelerator, but not actually
  // explicitly handle it. To this end we connect those menu items' accelerators
  // to this group, but don't attach this group to any top level window.
  GtkAccelGroup* dummy_accel_group_;

  // gtk_menu_popup() does not appear to take ownership of popup menus, so
  // MenuGtk explicitly manages the lifetime of the menu.
  GtkWidget* menu_;

  // True when we should ignore "activate" signals.  Used to prevent
  // menu items from getting activated when we are setting up the
  // menu.
  static bool block_activation_;

  // We must free these at shutdown.
  std::vector<MenuGtk*> submenus_we_own_;

  ScopedRunnableMethodFactory<MenuGtk> factory_;
};

#endif  // CHROME_BROWSER_GTK_MENU_GTK_H_
