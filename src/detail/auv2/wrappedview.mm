//  wrapped AUv2 view for clap-wrapper stub
//
//  created by Paul Walker (baconpaul) and Timo Kaluza (defiantnerd)

// since AUv2 uses a process wide(!) unique(!) class name for each editor type
// the wrapper's build helper creates those unique classes and puts them into
// the info.pList.

// we include the generated file here which configures the following C macros
// for each editor/plugin type:
//
//    CLAP_WRAPPER_COCOA_CLASS_NSVIEW   the unique class name for the view, derived from NSView
//    CLAP_WRAPPER_COCOA_CLASS          the unique class name for the AUBase, derived from AUCocoaUIBase
//    CLAP_WRAPPER_FILL_AUCV            the unique function name to create an matching view and cocoa class
//    CLAP_WRAPPER_TIMER_CALLBACK       the unique function name for a timer callback to maintain the view
//    CLAP_WRAPPER_EDITOR_NAME          the name of the plugin, used in the CocoaClass description method
//
// and then includes wrappedview.asinclude to flesh out the actual code.
//

#include "generated_cocoaclasses.hxx"
