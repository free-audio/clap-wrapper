#import "StandardMenuBar.h"

namespace freeaudio::clap_wrapper::standalone::macos
{
static NSMenuItem *addItem(NSMenu *menu, NSString *title, SEL action, NSString *key,
                           NSEventModifierFlags mods = NSEventModifierFlagCommand)
{
  // nil target: the action travels the responder chain, so plugin text fields get cut/copy/paste
  auto *item = [menu addItemWithTitle:title action:action keyEquivalent:key];
  item.keyEquivalentModifierMask = mods;
  return item;
}

static NSMenu *addSubmenu(NSMenu *bar, NSString *title)
{
  auto *menu = [[NSMenu alloc] initWithTitle:title];
  auto *holder = [bar addItemWithTitle:title action:nil keyEquivalent:@""];
  holder.submenu = menu;
  return menu;
}

static NSString *appName()
{
  NSString *name = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleName"];
  if (name.length == 0) name = NSProcessInfo.processInfo.processName;
  return name;
}

NSMenu *installStandardMenuBar(SEL settingsAction, NSString *settingsTitle)
{
  auto *name = appName();
  auto *bar = [[NSMenu alloc] initWithTitle:@"Main Menu"];

  // AppKit titles this menu with CFBundleName whatever we pass
  auto *app = addSubmenu(bar, name);
  addItem(app, [@"About " stringByAppendingString:name], @selector(orderFrontStandardAboutPanel:), @"");
  [app addItem:NSMenuItem.separatorItem];
  if (settingsAction)
  {
    addItem(app, settingsTitle, settingsAction, @",");
    [app addItem:NSMenuItem.separatorItem];
  }
  auto *services = [[NSMenu alloc] initWithTitle:@"Services"];
  addItem(app, @"Services", nil, @"").submenu = services;
  NSApp.servicesMenu = services;
  [app addItem:NSMenuItem.separatorItem];
  addItem(app, [@"Hide " stringByAppendingString:name], @selector(hide:), @"h");
  addItem(app, @"Hide Others", @selector(hideOtherApplications:), @"h",
          NSEventModifierFlagCommand | NSEventModifierFlagOption);
  addItem(app, @"Show All", @selector(unhideAllApplications:), @"");
  [app addItem:NSMenuItem.separatorItem];
  addItem(app, [@"Quit " stringByAppendingString:name], @selector(terminate:), @"q");

  auto *file = addSubmenu(bar, @"File");
  addItem(file, @"Close", @selector(performClose:), @"w");

  auto *edit = addSubmenu(bar, @"Edit");
  addItem(edit, @"Undo", @selector(undo:), @"z");
  addItem(edit, @"Redo", @selector(redo:), @"z", NSEventModifierFlagCommand | NSEventModifierFlagShift);
  [edit addItem:NSMenuItem.separatorItem];
  addItem(edit, @"Cut", @selector(cut:), @"x");
  addItem(edit, @"Copy", @selector(copy:), @"c");
  addItem(edit, @"Paste", @selector(paste:), @"v");
  addItem(edit, @"Select All", @selector(selectAll:), @"a");

  auto *window = addSubmenu(bar, @"Window");
  addItem(window, @"Minimize", @selector(performMiniaturize:), @"m");
  addItem(window, @"Zoom", @selector(performZoom:), @"");
  [window addItem:NSMenuItem.separatorItem];
  addItem(window, @"Bring All to Front", @selector(arrangeInFront:), @"");
  NSApp.windowsMenu = window;

  NSApp.helpMenu = addSubmenu(bar, @"Help");

  NSApp.mainMenu = bar;
  return file;
}
}  // namespace freeaudio::clap_wrapper::standalone::macos
