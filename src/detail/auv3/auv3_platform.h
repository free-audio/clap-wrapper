/*
    auv3_platform.h — platform compatibility shims for the AUv3 wrapper.

    Copyright (c) 2024 Timo Kaluza (defiantnerd) and the clap-wrappers contributors

    Released under the MIT License. See LICENSE for full license details.

    The AUv3 wrapper shares almost all of its code between macOS and iOS.
    A small set of APIs differ:

    * NSView vs UIView: type alias `CLAPWRAP_ViewClass`.
    * NSRect/NSSize vs CGRect/CGSize: `NSMake*` macros don't exist on iOS,
      so we use `CGRectMake` / `CGSizeMake` (which work on both platforms).
    * CLAP_WINDOW_API_COCOA vs CLAP_WINDOW_API_UIKIT.
    * NSView lifecycle names (`viewDidMoveToWindow` / `viewDidMoveToSuperview`)
      versus UIView (`didMoveToWindow` / `didMoveToSuperview`).

    This header centralises those shims so the rest of the wrapper can be
    written once. The UIKit window API identifier comes from CLAP itself
    (CLAP_WINDOW_API_UIKIT, "uikit", clap/ext/gui.h) - a hosted plugin
    must support it to show a UI on iOS.
*/

#pragma once

#include <TargetConditionals.h>
#include <clap/ext/gui.h>

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
typedef UIView CLAPWRAP_ViewClass;
#else
#import <AppKit/AppKit.h>
typedef NSView CLAPWRAP_ViewClass;
#endif

// Private CLAP window-API identifier for UIKit. Kept in sync with the
// constant of the same name in the hosted plugin's editor code.
//
// Newer CLAP declares CLAP_WINDOW_API_UIKIT as a variable in ext/gui.h; if
// this macro is defined before that header is parsed, it rewrites the
// declaration into garbage. Parse the header first — it is #pragma once, so
// it will not be parsed again after the macro exists.
#include <clap/clap.h>

#ifndef CLAP_WINDOW_API_UIKIT
#define CLAP_WINDOW_API_UIKIT "uikit"
#endif
