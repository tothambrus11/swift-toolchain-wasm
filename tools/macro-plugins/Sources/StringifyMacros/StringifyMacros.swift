// Example macros, the ones every macro tutorial starts with, used to test that
// a wasm-hosted compiler expands macros from a plugin it did not ship with.
// A program declares them itself:
//
//   @freestanding(expression)
//   macro stringify<T>(_ value: T) -> (T, String) =
//     #externalMacro(module: "StringifyMacros", type: "StringifyMacro")
//
//   @freestanding(expression)
//   macro diagnose(_ message: StaticString) -> Void =
//     #externalMacro(module: "StringifyMacros", type: "DiagnoseMacro")
import SwiftDiagnostics
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

/// `#stringify(x + 1)` expands to `(x + 1, "x + 1")`.
public struct StringifyMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> ExprSyntax {
    guard let argument = node.arguments.first?.expression else {
      throw MacroExpansionErrorMessage("#stringify needs an argument")
    }
    return "(\(argument), \(literal: argument.trimmedDescription))"
  }
}

/// `#diagnose("text")` expands to nothing and reports "text" as an error at
/// the macro, so a test can see a plugin's diagnostic reach the compiler's
/// output at the right place.
public struct DiagnoseMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> ExprSyntax {
    let message = node.arguments.first?.expression
      .as(StringLiteralExprSyntax.self)?.representedLiteralValue ?? "diagnose"
    context.diagnose(Diagnostic(node: node, message: MacroExpansionErrorMessage(message)))
    return "()"
  }
}
