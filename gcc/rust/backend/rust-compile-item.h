// Copyright (C) 2020 Free Software Foundation, Inc.

// This file is part of GCC.

// GCC is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3, or (at your option) any later
// version.

// GCC is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.

// You should have received a copy of the GNU General Public License
// along with GCC; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

#ifndef RUST_COMPILE_ITEM
#define RUST_COMPILE_ITEM

#include "rust-compile-base.h"
#include "rust-compile-tyty.h"
#include "rust-compile-var-decl.h"
#include "rust-compile-stmt.h"

namespace Rust {
namespace Compile {

class CompileItem : public HIRCompileBase
{
public:
  static void compile (HIR::Item *item, Context *ctx)
  {
    CompileItem compiler (ctx);
    item->accept_vis (compiler);
  }

  virtual ~CompileItem () {}

  void visit (HIR::Function &function)
  {
    // items can be forward compiled which means we may not need to invoke this
    // code
    Bfunction *lookup = nullptr;
    if (ctx->lookup_function_decl (function.get_mappings ().get_hirid (),
				   &lookup))
      {
	// has this been added to the list then it must be finished
	if (ctx->function_completed (lookup))
	  return;
      }

    TyTy::TyBase *fnType;
    if (!ctx->get_tyctx ()->lookup_type (function.get_mappings ().get_hirid (),
					 &fnType))
      {
	rust_fatal_error (function.locus, "failed to lookup function type");
	return;
      }

    // convert to the actual function type
    auto compiled_fn_type = TyTyCompile::compile (ctx->get_backend (), fnType);

    Bfunction *fndecl
      = ctx->get_backend ()->function (compiled_fn_type, function.function_name,
				       "" /* asm_name */, 0 /* flags */,
				       function.get_locus ());
    ctx->insert_function_decl (function.get_mappings ().get_hirid (), fndecl);

    // setup the params
    TyTy::TyBase *tyret = TyTyExtractRetFromFnType::compile (fnType);
    std::vector<TyTy::ParamType *> typarams
      = TyTyExtractParamsFromFnType::compile (fnType);
    std::vector<Bvariable *> param_vars;

    for (auto &it : typarams)
      {
	auto compiled_param
	  = TyTyCompileParam::compile (ctx->get_backend (), fndecl, it);
	param_vars.push_back (compiled_param);

	ctx->insert_var_decl (it->get_ref (), compiled_param);
      }

    if (!ctx->get_backend ()->function_set_parameters (fndecl, param_vars))
      {
	rust_fatal_error (function.get_locus (),
			  "failed to setup parameter variables");
	return;
      }

    // lookup locals
    auto block_expr = function.function_body.get ();
    auto body_mappings = block_expr->get_mappings ();

    Resolver::Rib *rib = nullptr;
    if (!ctx->get_resolver ()->find_name_rib (body_mappings.get_nodeid (),
					      &rib))
      {
	rust_fatal_error (function.get_locus (),
			  "failed to setup locals per block");
	return;
      }

    std::vector<Bvariable *> locals;
    rib->iterate_decls ([&] (NodeId n) mutable -> bool {
      Resolver::Definition d;
      bool ok = ctx->get_resolver ()->lookup_definition (n, &d);
      rust_assert (ok);

      HIR::Stmt *decl = nullptr;
      ok = ctx->get_mappings ()->resolve_nodeid_to_stmt (d.parent, &decl);
      rust_assert (ok);

      Bvariable *compiled = CompileVarDecl::compile (fndecl, decl, ctx);
      locals.push_back (compiled);

      return true;
    });

    bool toplevel_item
      = function.get_mappings ().get_local_defid () != UNKNOWN_LOCAL_DEFID;
    Bblock *enclosing_scope
      = toplevel_item ? NULL : ctx->peek_enclosing_scope ();

    HIR::BlockExpr *function_body = function.function_body.get ();
    Location start_location = function_body->get_locus ();
    Location end_location = function_body->get_closing_locus ();

    Bblock *code_block
      = ctx->get_backend ()->block (fndecl, enclosing_scope, locals,
				    start_location, end_location);
    ctx->push_block (code_block);

    Bvariable *return_address = nullptr;
    if (function.has_function_return_type ())
      {
	Btype *return_type = TyTyCompile::compile (ctx->get_backend (), tyret);

	bool address_is_taken = false;
	Bstatement *ret_var_stmt = nullptr;

	return_address = ctx->get_backend ()->temporary_variable (
	  fndecl, code_block, return_type, NULL, address_is_taken,
	  function.get_locus (), &ret_var_stmt);

	ctx->add_statement (ret_var_stmt);
      }

    ctx->push_fn (fndecl, return_address);

    // compile the block
    function_body->iterate_stmts ([&] (HIR::Stmt *s) mutable -> bool {
      CompileStmt::Compile (s, ctx);
      return true;
    });

    ctx->pop_block ();
    auto body = ctx->get_backend ()->block_statement (code_block);
    if (!ctx->get_backend ()->function_set_body (fndecl, body))
      {
	rust_error_at (function.get_locus (), "failed to set body to function");
	return;
      }

    ctx->pop_fn ();

    ctx->push_function (fndecl);
  }

private:
  CompileItem (Context *ctx) : HIRCompileBase (ctx) {}
};

} // namespace Compile
} // namespace Rust

#endif // RUST_COMPILE_ITEM
