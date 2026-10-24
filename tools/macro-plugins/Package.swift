// swift-tools-version:6.0
//
// The macro plugins a wasm-hosted swift-frontend runs, as one wasm32-wasip1
// executable sharing one swift-syntax: ObservationMacros (from the Swift
// repository), TestingMacros (from swift-testing) and StringifyMacros (an
// example, in this directory). scripts/37-macro-plugins.sh stages this file next
// to copies of those sources and builds it with the Swift SDK for WebAssembly;
// it is not meant to be built from here.
//
// A macro is found by the name of the module that implements it: the compiler
// asks for "ObservationMacros.ObservableMacro", and swift-syntax's
// CompilerPlugin compares that with String(reflecting:) of each type it
// provides. So each plugin keeps its own module, as a library target, and only
// the entry point (SwiftMacroPlugins/SwiftMacroPlugins.swift, generated) is
// shared.
import Foundation
import PackageDescription

let swiftSyntaxPath = Context.environment["SWIFT_SYNTAX_PATH"] ?? "../swift-syntax"

let syntax: [Target.Dependency] = [
  .product(name: "SwiftDiagnostics", package: "swift-syntax"),
  .product(name: "SwiftSyntax", package: "swift-syntax"),
  .product(name: "SwiftSyntaxBuilder", package: "swift-syntax"),
  .product(name: "SwiftSyntaxMacros", package: "swift-syntax"),
]

let package = Package(
  name: "SwiftMacroPlugins",
  dependencies: [.package(path: swiftSyntaxPath)],
  targets: [
    .target(name: "ObservationMacros", dependencies: syntax),
    .target(
      name: "TestingMacros",
      dependencies: syntax + [
        .product(name: "SwiftParser", package: "swift-syntax"),
        .product(name: "SwiftIfConfig", package: "swift-syntax"),
        .product(name: "SwiftSyntaxMacroExpansion", package: "swift-syntax"),
        .product(name: "SwiftCompilerPlugin", package: "swift-syntax"),
      ],
      // What swift-testing's own package builds TestingMacros with.
      swiftSettings: [
        .enableUpcomingFeature("ExistentialAny"),
        .enableExperimentalFeature("AccessLevelOnImport"),
        .enableUpcomingFeature("InternalImportsByDefault"),
        .enableUpcomingFeature("MemberImportVisibility"),
        .enableExperimentalFeature("Lifetimes"),
        .enableUpcomingFeature("InferIsolatedConformances"),
      ]
    ),
    .target(name: "StringifyMacros", dependencies: syntax),
    .executableTarget(
      name: "SwiftMacroPlugins",
      dependencies: [
        "ObservationMacros", "TestingMacros", "StringifyMacros",
        .product(name: "SwiftCompilerPlugin", package: "swift-syntax"),
      ]
    ),
  ]
)
