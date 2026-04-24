/*
    auv3_platform.h — platform compatibility shims for the AUv3 wrapper.

    Copyright (c) 2024 Timo Kaluza (defiantnerd) and the clap-wrappers contributors

    Released under the MIT License. See LICENSE for full license details.

    The AUv3 wrapper shares almost all of its code between macOS and iOS.
    A small set of APIs differ:

    * NSView vs UIView: type alias `CLAPWRAP_ViewClass`.
    * NSRect/NSSize vs CGRect/CGSize: `NSMake*` macros don't exist on iOS,
      so we use `CGRectMake` / `CGSizeMake` (which work on both platforms).
    * CLAP_WINDOW_API_COCOA vs a private UIKit identifier.
    * NSView lifecycle names (`viewDidMoveToWindow` / `viewDidMoveToSuperview`)
      versus UIView (`didMoveToWindow` / `didMoveToSuperview`).

    This header centralises those shims so the rest of the wrapper can be
    written once. CLAP does not (yet) define a UIKit window API identifier
    in clap/ext/gui.h; until it does, we use a private string "uikit" and
    expect the hosted plugin to match (plugins that use clap-wrapper on
    iOS must recognise this string on their side).
*/

#pragma once

#include <TargetConditionals.h>

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
typedef UIView CLAPWRAP_ViewClass;
#else
#import <AppKit/AppKit.h>
typedef NSView CLAPWRAP_ViewClass;
#endif

// Private CLAP window-API identifier for UIKit. Kept in sync with the
// constant of the same name in the hosted plugin's editor code.
#ifndef CLAP_WINDOW_API_UIKIT
#define CLAP_WINDOW_API_UIKIT "uikit"
#endif
