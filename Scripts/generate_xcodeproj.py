#!/usr/bin/env python3
"""Generates IpaStore.xcodeproj from the files on disk.

Keeping the project file generated makes file additions mechanical and keeps
the hand-written pbxproj free of stale references. Run this script after adding
or removing a source file:

    python3 Scripts/generate_xcodeproj.py
"""

from __future__ import annotations

import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT / "IpaStore.xcodeproj"
TARGET_NAME = "IpaStore"
BUNDLE_ID = "com.example.ipastore"


def ident(key: str) -> str:
    return hashlib.md5(key.encode("utf-8")).hexdigest().upper()[:24]


def quote(value: str) -> str:
    if value == "":
        return '""'
    if all(character.isalnum() or character in "._/$" for character in value):
        return value
    return '"' + value.replace('"', '\\"') + '"'


class FileRef:
    def __init__(self, path: Path, file_type: str, group: str, build_phase: str | None, name: str | None = None):
        self.path = path
        self.file_type = file_type
        self.group = group
        self.build_phase = build_phase
        self.name = name or path.name
        self.key = str(path.relative_to(ROOT))
        self.ref = ident("fileref:" + self.key)
        self.build = ident("build:" + self.key)

    def reference_line(self) -> str:
        return (
            f"\t\t{self.ref} /* {self.name} */ = {{isa = PBXFileReference; "
            f"lastKnownFileType = {self.file_type}; path = {quote(self.name)}; "
            f"sourceTree = \"<group>\"; }};\n"
        )


def collect() -> list[FileRef]:
    files: list[FileRef] = []

    for path in sorted((ROOT / "IpaStore" / "Sources").glob("*.swift")):
        files.append(FileRef(path, "sourcecode.swift", "sources", "sources"))

    for path in sorted((ROOT / "IpaStore" / "Bridge").glob("*.mm")):
        files.append(FileRef(path, "sourcecode.cpp.objcpp", "bridge", "sources"))
    for path in sorted((ROOT / "IpaStore" / "Bridge").glob("*.h")):
        files.append(FileRef(path, "sourcecode.c.h", "bridge", None))

    files.append(FileRef(ROOT / "IpaStore" / "IpaStore-Bridging-Header.h",
                         "sourcecode.c.h", "ipa", None))

    for path in sorted((ROOT / "Core" / "include" / "ipashelf").glob("*.hpp")):
        files.append(FileRef(path, "sourcecode.c.h", "ipashelf", None))
    # The sources live in Core/src, so they need their own group: a file
    # reference's path is resolved relative to its group.
    for path in sorted((ROOT / "Core" / "src").glob("*.cpp")):
        files.append(FileRef(path, "sourcecode.cpp.cpp", "src", "sources"))

    for path in sorted((ROOT / "ThirdParty" / "miniz").glob("*.c")):
        files.append(FileRef(path, "sourcecode.c.c", "miniz", "sources"))
    for path in sorted((ROOT / "ThirdParty" / "miniz").glob("*.h")):
        files.append(FileRef(path, "sourcecode.c.h", "miniz", None))

    assets = ROOT / "IpaStore" / "Resources" / "Assets.xcassets"
    if assets.exists():
        files.append(FileRef(assets, "folder.assetcatalog", "resources", "resources"))

    install_icon = ROOT / "IpaStore" / "Resources" / "InstallIcon.png"
    if install_icon.exists():
        files.append(FileRef(install_icon, "image.png", "resources", "resources"))

    privacy = ROOT / "IpaStore" / "Resources" / "PrivacyInfo.xcprivacy"
    if privacy.exists():
        files.append(FileRef(privacy, "text.xml", "resources", "resources"))

    files.append(FileRef(ROOT / "Config" / "Info.plist", "text.plist.xml", "config", None))
    files.append(FileRef(ROOT / "Config" / "IpaStore.entitlements",
                         "text.plist.entitlements", "config", None))
    return files


def group_block(gid: str, name: str, path: str | None, children: list[str], comment: str) -> str:
    child_lines = "".join(f"\t\t\t\t{child},\n" for child in children)
    path_line = f"\t\t\tpath = {quote(path)};\n" if path else ""
    return (
        f"\t\t{gid} /* {comment} */ = {{\n"
        f"\t\t\tisa = PBXGroup;\n"
        f"\t\t\tchildren = (\n{child_lines}\t\t\t);\n"
        f"\t\t\tname = {quote(name)};\n"
        f"{path_line}"
        f"\t\t\tsourceTree = \"<group>\";\n"
        f"\t\t}};\n"
    )


def build_settings(target: bool, release: bool) -> str:
    common = {
        "ALWAYS_SEARCH_USER_PATHS": "NO",
        "CLANG_ANALYZER_NONNULL": "YES",
        "CLANG_CXX_LANGUAGE_STANDARD": '"c++20"',
        "CLANG_CXX_LIBRARY": '"libc++"',
        "CLANG_ENABLE_MODULES": "YES",
        "CLANG_ENABLE_OBJC_ARC": "YES",
        "CLANG_ENABLE_OBJC_WEAK": "YES",
        "CLANG_WARN_BLOCK_CAPTURE_AUTORELEASING": "YES",
        "CLANG_WARN_BOOL_CONVERSION": "YES",
        "CLANG_WARN_COMMA": "YES",
        "CLANG_WARN_CONSTANT_CONVERSION": "YES",
        "CLANG_WARN_DEPRECATED_OBJC_IMPLEMENTATIONS": "YES",
        "CLANG_WARN_DIRECT_OBJC_ISA_USAGE": "YES_ERROR",
        "CLANG_WARN_DOCUMENTATION_COMMENTS": "YES",
        "CLANG_WARN_EMPTY_BODY": "YES",
        "CLANG_WARN_ENUM_CONVERSION": "YES",
        "CLANG_WARN_INFINITE_RECURSION": "YES",
        "CLANG_WARN_INT_CONVERSION": "YES",
        "CLANG_WARN_NON_LITERAL_NULL_CONVERSION": "YES",
        "CLANG_WARN_OBJC_IMPLICIT_RETAIN_SELF": "YES",
        "CLANG_WARN_OBJC_LITERAL_CONVERSION": "YES",
        "CLANG_WARN_OBJC_ROOT_CLASS": "YES_ERROR",
        "CLANG_WARN_QUOTED_INCLUDE_IN_FRAMEWORK_HEADER": "YES",
        "CLANG_WARN_RANGE_LOOP_ANALYSIS": "YES",
        "CLANG_WARN_STRICT_PROTOTYPES": "YES",
        "CLANG_WARN_SUSPICIOUS_MOVE": "YES",
        "CLANG_WARN_UNGUARDED_AVAILABILITY": "YES_AGGRESSIVE",
        "CLANG_WARN_UNREACHABLE_CODE": "YES",
        "CLANG_WARN__DUPLICATE_METHOD_MATCH": "YES",
        "COPY_PHASE_STRIP": "NO",
        "ENABLE_STRICT_OBJC_MSGSEND": "YES",
        "ENABLE_USER_SCRIPT_SANDBOXING": "YES",
        "GCC_C_LANGUAGE_STANDARD": "gnu17",
        "GCC_NO_COMMON_BLOCKS": "YES",
        "GCC_WARN_64_TO_32_BIT_CONVERSION": "YES",
        "GCC_WARN_ABOUT_RETURN_TYPE": "YES_ERROR",
        "GCC_WARN_UNDECLARED_SELECTOR": "YES",
        "GCC_WARN_UNINITIALIZED_AUTOS": "YES_AGGRESSIVE",
        "GCC_WARN_UNUSED_FUNCTION": "YES",
        "GCC_WARN_UNUSED_VARIABLE": "YES",
        "IPHONEOS_DEPLOYMENT_TARGET": "27.0",
        "LOCALIZATION_PREFERS_STRING_CATALOGS": "YES",
        "MTL_FAST_MATH": "YES",
        "SDKROOT": "iphoneos",
        "SWIFT_STRICT_CONCURRENCY": "minimal",
        "SWIFT_VERSION": "5.0",
    }

    if target:
        common.update({
            "ASSETCATALOG_COMPILER_APPICON_NAME": "AppIcon",
            "ASSETCATALOG_COMPILER_GLOBAL_ACCENT_COLOR_NAME": "AccentColor",
            "CODE_SIGN_ENTITLEMENTS": "Config/IpaStore.entitlements",
            "CODE_SIGN_STYLE": "Automatic",
            "CURRENT_PROJECT_VERSION": "1",
            "DEVELOPMENT_TEAM": '""',
            "ENABLE_PREVIEWS": "YES",
            "GENERATE_INFOPLIST_FILE": "NO",
            "HEADER_SEARCH_PATHS": '(\n\t\t\t\t\t"$(SRCROOT)/Core/include",\n\t\t\t\t\t"$(SRCROOT)/ThirdParty/miniz",\n\t\t\t\t)', 
            "INFOPLIST_FILE": "Config/Info.plist",
            "LD_RUNPATH_SEARCH_PATHS": '(\n\t\t\t\t\t"$(inherited)",\n\t\t\t\t\t"@executable_path/Frameworks",\n\t\t\t\t)',
            "MARKETING_VERSION": "0.1.0",
            "PRODUCT_BUNDLE_IDENTIFIER": BUNDLE_ID,
            "PRODUCT_NAME": '"$(TARGET_NAME)"',
            "SUPPORTS_MACCATALYST": "NO",
            "SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD": "NO",
            "SWIFT_EMIT_LOC_STRINGS": "YES",
            "SWIFT_OBJC_BRIDGING_HEADER": "IpaStore/IpaStore-Bridging-Header.h",
            "TARGETED_DEVICE_FAMILY": '"1,2"',
        })
        if release:
            common.update({
                "SWIFT_COMPILATION_MODE": "wholemodule",
                "SWIFT_OPTIMIZATION_LEVEL": '"-O"',
                "VALIDATE_PRODUCT": "YES",
            })
        else:
            common.update({
                "DEBUG_INFORMATION_FORMAT": "dwarf",
                "ENABLE_TESTABILITY": "YES",
                "ONLY_ACTIVE_ARCH": "YES",
                "SWIFT_ACTIVE_COMPILATION_CONDITIONS": '"DEBUG $(inherited)"',
                "SWIFT_OPTIMIZATION_LEVEL": '"-Onone"',
            })
    else:
        if release:
            common.update({
                "DEBUG_INFORMATION_FORMAT": '"dwarf-with-dsym"',
                "ENABLE_NS_ASSERTIONS": "NO",
                "VALIDATE_PRODUCT": "YES",
            })
        else:
            common.update({
                "DEBUG_INFORMATION_FORMAT": "dwarf",
                "ENABLE_TESTABILITY": "YES",
                "GCC_OPTIMIZATION_LEVEL": "0",
                "GCC_PREPROCESSOR_DEFINITIONS": '"DEBUG=1 $(inherited)"',
                "ONLY_ACTIVE_ARCH": "YES",
                "SWIFT_ACTIVE_COMPILATION_CONDITIONS": '"DEBUG $(inherited)"',
            })

    lines = []
    for key in sorted(common):
        lines.append(f"\t\t\t\t{key} = {common[key]};\n")
    return "".join(lines)


def main() -> None:
    files = collect()

    groups = {
        "ipa": ident("group:ipa"),
        "sources": ident("group:sources"),
        "bridge": ident("group:bridge"),
        "resources": ident("group:resources"),
        "config": ident("group:config"),
        "core": ident("group:core"),
        "src": ident("group:src"),
        "include": ident("group:include"),
        "ipashelf": ident("group:ipashelf"),
        "thirdparty": ident("group:thirdparty"),
        "miniz": ident("group:miniz"),
        "products": ident("group:products"),
        "frameworks": ident("group:frameworks"),
        "main": ident("group:main"),
    }

    children: dict[str, list[str]] = {key: [] for key in groups}
    for ref in files:
        if ref.group == "ipa":
            children["ipa"].append(f"{ref.ref} /* {ref.name} */")
        else:
            children[ref.group].append(f"{ref.ref} /* {ref.name} */")

    product_ref = ident("product:app")
    target_id = ident("target:app")
    project_id = ident("project:root")
    sources_phase = ident("phase:sources")
    frameworks_phase = ident("phase:frameworks")
    resources_phase = ident("phase:resources")
    project_config_list = ident("configlist:project")
    target_config_list = ident("configlist:target")
    project_debug = ident("config:project:debug")
    project_release = ident("config:project:release")
    target_debug = ident("config:target:debug")
    target_release = ident("config:target:release")
    security_ref = ident("framework:security")
    security_build = ident("build:security")
    network_ref = ident("framework:network")
    network_build = ident("build:network")

    # Group children: nested groups first.
    children["sources"] = sorted(children["sources"])
    children["bridge"] = sorted(children["bridge"])
    children["ipashelf"] = sorted(children["ipashelf"])
    children["src"] = sorted(children["src"])
    children["miniz"] = sorted(children["miniz"])
    children["resources"] = sorted(children["resources"])
    children["config"] = sorted(children["config"])
    children["include"] = [f"{groups['ipashelf']} /* ipashelf */"]
    children["core"] = [
        f"{groups['include']} /* include */",
        f"{groups['src']} /* src */",
    ]
    children["miniz"] = sorted(children["miniz"])
    children["thirdparty"] = [f"{groups['miniz']} /* miniz */"]
    children["ipa"] = [
        f"{groups['sources']} /* Sources */",
        f"{groups['bridge']} /* Bridge */",
        f"{groups['resources']} /* Resources */",
    ] + children["ipa"]
    children["products"] = [f"{product_ref} /* {TARGET_NAME}.app */"]
    children["frameworks"] = [
        f"{security_ref} /* Security.framework */",
        f"{network_ref} /* Network.framework */",
    ]
    children["main"] = [
        f"{groups['ipa']} /* IpaStore */",
        f"{groups['core']} /* Core */",
        f"{groups['thirdparty']} /* ThirdParty */",
        f"{groups['config']} /* Config */",
        f"{groups['frameworks']} /* Frameworks */",
        f"{groups['products']} /* Products */",
    ]

    group_text = ""
    group_text += group_block(groups["main"], "IpaStore", None, children["main"], "Main")
    group_text += group_block(groups["ipa"], "IpaStore", "IpaStore", children["ipa"], "IpaStore")
    group_text += group_block(groups["sources"], "Sources", "Sources", children["sources"], "Sources")
    group_text += group_block(groups["bridge"], "Bridge", "Bridge", children["bridge"], "Bridge")
    group_text += group_block(groups["resources"], "Resources", "Resources", children["resources"], "Resources")
    group_text += group_block(groups["config"], "Config", "Config", children["config"], "Config")
    group_text += group_block(groups["core"], "Core", "Core", children["core"], "Core")
    group_text += group_block(groups["src"], "src", "src", children["src"], "src")
    group_text += group_block(groups["include"], "include", "include", children["include"], "include")
    group_text += group_block(groups["ipashelf"], "ipashelf", "ipashelf", children["ipashelf"], "ipashelf")
    group_text += group_block(groups["thirdparty"], "ThirdParty", "ThirdParty", children["thirdparty"], "ThirdParty")
    group_text += group_block(groups["miniz"], "miniz", "miniz", children["miniz"], "miniz")
    group_text += group_block(groups["products"], "Products", None, children["products"], "Products")
    group_text += group_block(groups["frameworks"], "Frameworks", None, children["frameworks"], "Frameworks")

    file_ref_text = "".join(ref.reference_line() for ref in files)
    file_ref_text += (
        f"\t\t{product_ref} /* {TARGET_NAME}.app */ = {{isa = PBXFileReference; "
        f"explicitFileType = wrapper.application; includeInIndex = 0; "
        f"path = {TARGET_NAME}.app; sourceTree = BUILT_PRODUCTS_DIR; }};\n"
    )
    for framework_ref, framework_name in (
        (security_ref, "Security.framework"),
        (network_ref, "Network.framework"),
    ):
        file_ref_text += (
            f"\t\t{framework_ref} /* {framework_name} */ = {{isa = PBXFileReference; "
            f"lastKnownFileType = wrapper.framework; name = {framework_name}; "
            f"path = System/Library/Frameworks/{framework_name}; "
            f"sourceTree = SDKROOT; }};\n"
        )

    build_text = ""
    for ref in files:
        if ref.build_phase is None:
            continue
        phase_name = "Sources" if ref.build_phase == "sources" else "Resources"
        build_text += (
            f"\t\t{ref.build} /* {ref.name} in {phase_name} */ = {{isa = PBXBuildFile; "
            f"fileRef = {ref.ref} /* {ref.name} */; }};\n"
        )
    for framework_build, framework_ref, framework_name in (
        (security_build, security_ref, "Security.framework"),
        (network_build, network_ref, "Network.framework"),
    ):
        build_text += (
            f"\t\t{framework_build} /* {framework_name} in Frameworks */ = "
            f"{{isa = PBXBuildFile; fileRef = {framework_ref} /* {framework_name} */; }};\n"
        )

    source_files = "".join(
        f"\t\t\t\t{ref.build} /* {ref.name} in Sources */,\n"
        for ref in files if ref.build_phase == "sources"
    )
    resource_files = "".join(
        f"\t\t\t\t{ref.build} /* {ref.name} in Resources */,\n"
        for ref in files if ref.build_phase == "resources"
    )

    pbx = f"""// !$*UTF8*$!
{{
	archiveVersion = 1;
	classes = {{
	}};
	objectVersion = 77;
	objects = {{

/* Begin PBXBuildFile section */
{build_text}/* End PBXBuildFile section */

/* Begin PBXFileReference section */
{file_ref_text}/* End PBXFileReference section */

/* Begin PBXFrameworksBuildPhase section */
		{frameworks_phase} /* Frameworks */ = {{
			isa = PBXFrameworksBuildPhase;
			buildActionMask = 2147483647;
			files = (
				{security_build} /* Security.framework in Frameworks */,
				{network_build} /* Network.framework in Frameworks */,
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXFrameworksBuildPhase section */

/* Begin PBXGroup section */
{group_text}/* End PBXGroup section */

/* Begin PBXNativeTarget section */
		{target_id} /* {TARGET_NAME} */ = {{
			isa = PBXNativeTarget;
			buildConfigurationList = {target_config_list} /* Build configuration list for PBXNativeTarget "{TARGET_NAME}" */;
			buildPhases = (
				{sources_phase} /* Sources */,
				{frameworks_phase} /* Frameworks */,
				{resources_phase} /* Resources */,
			);
			buildRules = (
			);
			dependencies = (
			);
			name = {TARGET_NAME};
			productName = {TARGET_NAME};
			productReference = {product_ref} /* {TARGET_NAME}.app */;
			productType = "com.apple.product-type.application";
		}};
/* End PBXNativeTarget section */

/* Begin PBXProject section */
		{project_id} /* Project object */ = {{
			isa = PBXProject;
			attributes = {{
				BuildIndependentTargetsInParallel = 1;
				LastSwiftUpdateCheck = 2700;
				LastUpgradeCheck = 2700;
				TargetAttributes = {{
					{target_id} = {{
						CreatedOnToolsVersion = 27.0;
					}};
				}};
			}};
			buildConfigurationList = {project_config_list} /* Build configuration list for PBXProject "{TARGET_NAME}" */;
			compatibilityVersion = "Xcode 16.0";
			developmentRegion = en;
			hasScannedForEncodings = 0;
			knownRegions = (
				en,
				Base,
			);
			mainGroup = {groups['main']};
			minimizedProjectReferenceProxies = 1;
			preferredProjectObjectVersion = 77;
			productRefGroup = {groups['products']} /* Products */;
			projectDirPath = "";
			projectRoot = "";
			targets = (
				{target_id} /* {TARGET_NAME} */,
			);
		}};
/* End PBXProject section */

/* Begin PBXResourcesBuildPhase section */
		{resources_phase} /* Resources */ = {{
			isa = PBXResourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
{resource_files}			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXResourcesBuildPhase section */

/* Begin PBXSourcesBuildPhase section */
		{sources_phase} /* Sources */ = {{
			isa = PBXSourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
{source_files}			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXSourcesBuildPhase section */

/* Begin XCBuildConfiguration section */
		{project_debug} /* Debug */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
{build_settings(False, False)}			}};
			name = Debug;
		}};
		{project_release} /* Release */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
{build_settings(False, True)}			}};
			name = Release;
		}};
		{target_debug} /* Debug */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
{build_settings(True, False)}			}};
			name = Debug;
		}};
		{target_release} /* Release */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
{build_settings(True, True)}			}};
			name = Release;
		}};
/* End XCBuildConfiguration section */

/* Begin XCConfigurationList section */
		{project_config_list} /* Build configuration list for PBXProject "{TARGET_NAME}" */ = {{
			isa = XCConfigurationList;
			buildConfigurations = (
				{project_debug} /* Debug */,
				{project_release} /* Release */,
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		}};
		{target_config_list} /* Build configuration list for PBXNativeTarget "{TARGET_NAME}" */ = {{
			isa = XCConfigurationList;
			buildConfigurations = (
				{target_debug} /* Debug */,
				{target_release} /* Release */,
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		}};
/* End XCConfigurationList section */
	}};
	rootObject = {project_id} /* Project object */;
}}
"""

    PROJECT.mkdir(parents=True, exist_ok=True)
    (PROJECT / "project.pbxproj").write_text(pbx, encoding="utf-8")

    workspace = PROJECT / "project.xcworkspace"
    workspace.mkdir(parents=True, exist_ok=True)
    (workspace / "contents.xcworkspacedata").write_text(
        """<?xml version="1.0" encoding="UTF-8"?>
<Workspace
   version = "1.0">
   <FileRef
      location = "self:">
   </FileRef>
</Workspace>
""",
        encoding="utf-8",
    )

    shared = PROJECT / "xcshareddata" / "xcschemes"
    shared.mkdir(parents=True, exist_ok=True)
    (shared / f"{TARGET_NAME}.xcscheme").write_text(
        f"""<?xml version="1.0" encoding="UTF-8"?>
<Scheme
   LastUpgradeVersion = "2700"
   version = "1.7">
   <BuildAction
      parallelizeBuildables = "YES"
      buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry
            buildForTesting = "YES"
            buildForRunning = "YES"
            buildForProfiling = "YES"
            buildForArchiving = "YES"
            buildForAnalyzing = "YES">
            <BuildableReference
               BuildableIdentifier = "primary"
               BlueprintIdentifier = "{target_id}"
               BuildableName = "{TARGET_NAME}.app"
               BlueprintName = "{TARGET_NAME}"
               ReferencedContainer = "container:{TARGET_NAME}.xcodeproj">
            </BuildableReference>
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <TestAction
      buildConfiguration = "Debug"
      selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB"
      selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB"
      shouldUseLaunchSchemeArgsEnv = "YES">
      <Testables>
      </Testables>
   </TestAction>
   <LaunchAction
      buildConfiguration = "Debug"
      selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB"
      selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB"
      launchStyle = "0"
      useCustomWorkingDirectory = "NO"
      ignoresPersistentStateOnLaunch = "NO"
      debugDocumentVersioning = "YES"
      debugServiceExtension = "internal"
      allowLocationSimulation = "YES">
      <BuildableProductRunnable
         runnableDebuggingMode = "0">
         <BuildableReference
            BuildableIdentifier = "primary"
            BlueprintIdentifier = "{target_id}"
            BuildableName = "{TARGET_NAME}.app"
            BlueprintName = "{TARGET_NAME}"
            ReferencedContainer = "container:{TARGET_NAME}.xcodeproj">
         </BuildableReference>
      </BuildableProductRunnable>
   </LaunchAction>
   <ProfileAction
      buildConfiguration = "Release"
      shouldUseLaunchSchemeArgsEnv = "YES"
      savedToolIdentifier = ""
      useCustomWorkingDirectory = "NO"
      debugDocumentVersioning = "YES">
      <BuildableProductRunnable
         runnableDebuggingMode = "0">
         <BuildableReference
            BuildableIdentifier = "primary"
            BlueprintIdentifier = "{target_id}"
            BuildableName = "{TARGET_NAME}.app"
            BlueprintName = "{TARGET_NAME}"
            ReferencedContainer = "container:{TARGET_NAME}.xcodeproj">
         </BuildableReference>
      </BuildableProductRunnable>
   </ProfileAction>
   <AnalyzeAction
      buildConfiguration = "Debug">
   </AnalyzeAction>
   <ArchiveAction
      buildConfiguration = "Release"
      revealArchiveInOrganizer = "YES">
   </ArchiveAction>
</Scheme>
""",
        encoding="utf-8",
    )

    print(f"wrote {PROJECT}")
    print(f"  {len(files)} file references")


if __name__ == "__main__":
    main()
