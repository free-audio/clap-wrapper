# automatically build the wrapper only if this is the top level project
if (PROJECT_IS_TOP_LEVEL)
	# provide the CLAP_WRAPPER_OUTPUT_NAME to specify the matching plugin name.
	if((NOT CLAP_WRAPPER_OUTPUT_NAME ) OR (CLAP_WRAPPER_OUTPUT_NAME STREQUAL ""))
		set(CLAP_WRAPPER_OUTPUT_NAME "clapasvst3")
		message(WARNING "clap-wrapper: CLAP_WRAPPER_OUTPUT_NAME not set - continuing with a default name `clapasvst3`")
	endif()

	string(MAKE_C_IDENTIFIER ${CLAP_WRAPPER_OUTPUT_NAME} pluginname)


	# AAX is opt-in like AUv2/AUv3, and only available where the platform/toolchain
	# supports it (macOS and non-ARM MSVC Windows, never Linux). Requiring both the
	# opt-in and the capability avoids fetching the AAX SDK when it was not asked for.
	if (CLAP_WRAPPER_BUILD_AAX)
		if (CLAP_WRAPPER_CAN_BUILD_AAX)
			# Link the actual plugin library
			add_library(${pluginname}_as_aax MODULE)
			target_add_aax_wrapper(
					TARGET ${pluginname}_as_aax
					OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME}"
					BUNDLE_IDENTIFIER "${CLAP_WRAPPER_BUNDLE_IDENTIFIER}"
					BUNDLE_VERSION "${CLAP_WRAPPER_BUNDLE_VERSION}"
			)
		else()
			message(WARNING "clap-wrapper: CLAP_WRAPPER_BUILD_AAX was requested but AAX is not "
					"supported on this platform/toolchain - skipping the AAX wrapper")
		endif()
	endif()

	# Link the actual plugin library
	add_library(${pluginname}_as_vst3 MODULE)
	target_add_vst3_wrapper(
			TARGET ${pluginname}_as_vst3
			OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME}"
			SUPPORTS_ALL_NOTE_EXPRESSIONS $<BOOL:${CLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS}>
			SINGLE_PLUGIN_TUID "${CLAP_VST3_TUID_STRING}"
			BUNDLE_IDENTIFIER "${CLAP_WRAPPER_BUNDLE_IDENTIFIER}"
			BUNDLE_VERSION "${CLAP_WRAPPER_BUNDLE_VERSION}"
			)

	if (APPLE)
		if (${CLAP_WRAPPER_BUILD_AUV2})
			add_library(${pluginname}_as_auv2 MODULE)
			target_add_auv2_wrapper(
					TARGET ${pluginname}_as_auv2
					OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME}"
					BUNDLE_IDENTIFIER "${CLAP_WRAPPER_BUNDLE_IDENTIFIER}"
					BUNDLE_VERSION "${CLAP_WRAPPER_BUNDLE_VERSION}"

					# AUv2 is still a work in progress. For now make this an
					# explict option to the single plugin version
					INSTRUMENT_TYPE "aumu"
					MANUFACTURER_NAME "cleveraudio.org"
					MANUFACTURER_CODE "clAd"
					SUBTYPE_CODE "gWrp"
			)
		endif()
	endif()

	if (APPLE)
		if (${CLAP_WRAPPER_BUILD_AUV3})
			# One set of component codes for the appex and the standalone host —
			# the host looks its embedded appex up via AudioComponentFindNext,
			# so these MUST match the codes the appex registers with.
			set(_auv3_type "aumu")
			set(_auv3_subtype "gWrq")
			set(_auv3_manufacturer "clAd")

			# The appex and the host app need distinct bundle identifiers
			# (identical IDs break appex registration). Keep them empty when no
			# identifier was configured so each wrapper generates its own
			# unique default.
			if ("${CLAP_WRAPPER_BUNDLE_IDENTIFIER}" STREQUAL "")
				set(_auv3_appex_id "")
				set(_auv3_host_id "")
			else()
				set(_auv3_appex_id "${CLAP_WRAPPER_BUNDLE_IDENTIFIER}.auv3")
				set(_auv3_host_id "${CLAP_WRAPPER_BUNDLE_IDENTIFIER}.auv3standalone")
			endif()

			add_executable(${pluginname}_as_auv3)
			target_add_auv3_wrapper(
					TARGET ${pluginname}_as_auv3
					OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME}"
					BUNDLE_IDENTIFIER "${_auv3_appex_id}"
					BUNDLE_VERSION "${CLAP_WRAPPER_BUNDLE_VERSION}"

					INSTRUMENT_TYPE "${_auv3_type}"
					MANUFACTURER_NAME "cleveraudio.org"
					MANUFACTURER_CODE "${_auv3_manufacturer}"
					SUBTYPE_CODE "${_auv3_subtype}"
			)

			# Embed the installed .clap into the appex so it can find the plugin at runtime
			set(_clap_bundle_name "${CLAP_WRAPPER_OUTPUT_NAME}.clap")
			set(_clap_embed_dst "$<TARGET_BUNDLE_DIR:${pluginname}_as_auv3>/Contents/PlugIns/${_clap_bundle_name}")
			add_custom_command(TARGET ${pluginname}_as_auv3 POST_BUILD
					COMMAND ${CMAKE_COMMAND} "-DCLAP_NAME=${_clap_bundle_name}" "-DDST=${_clap_embed_dst}"
						-P "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_clap.cmake"
					COMMENT "Embedding ${_clap_bundle_name} in AUv3 appex"
			)

			add_executable(${pluginname}_as_auv3_standalone)
			target_add_auv3_standalone_wrapper(
					TARGET ${pluginname}_as_auv3_standalone
					OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME} AUv3"
					BUNDLE_IDENTIFIER "${_auv3_host_id}"
					BUNDLE_VERSION "${CLAP_WRAPPER_BUNDLE_VERSION}"
					AUV3_TARGET ${pluginname}_as_auv3
					AU_TYPE "${_auv3_type}"
					AU_SUBTYPE "${_auv3_subtype}"
					AU_MANUFACTURER "${_auv3_manufacturer}"
			)
		endif()
	endif()

	if (${CLAP_WRAPPER_BUILD_STANDALONE})
		add_executable(${pluginname}_as_standalone)
		target_add_standalone_wrapper(TARGET ${pluginname}_as_standalone
		    OUTPUT_NAME ${CLAP_WRAPPER_OUTPUT_NAME}
		    HOSTED_CLAP_NAME ${CLAP_WRAPPER_OUTPUT_NAME}
		    PLUGIN_INDEX 0)
	endif()
endif()
