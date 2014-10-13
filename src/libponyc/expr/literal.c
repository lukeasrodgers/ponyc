#include "literal.h"
#include "../ast/token.h"
#include "../ds/stringtab.h"
#include "../pass/names.h"
#include "../type/subtype.h"
#include "../type/assemble.h"
#include "../type/assemble.h"
#include "../type/reify.h"
#include "../type/cap.h"
#include "../type/viewpoint.h"
#include <string.h>
#include <assert.h>

bool expr_literal(ast_t* ast, const char* name)
{
  ast_t* type = type_builtin(ast, name);

  if(type == NULL)
    return false;

  ast_settype(ast, type);
  return true;
}

bool expr_this(ast_t* ast)
{
  // TODO: If in a recover expression, may not have access to "this".
  // Or we could lower it to tag, since it can't be assigned to. If in a
  // constructor, lower it to tag if not all fields are defined.
  ast_t* type = type_for_this(ast, cap_for_receiver(ast), false);
  ast_settype(ast, type);

  ast_t* nominal;

  if(ast_id(type) == TK_NOMINAL)
    nominal = type;
  else
    nominal = ast_childidx(type, 1);

  ast_t* typeargs = ast_childidx(nominal, 2);
  ast_t* typearg = ast_child(typeargs);

  while(typearg != NULL)
  {
    if(!expr_nominal(&typearg))
    {
      ast_error(ast, "couldn't create a type for 'this'");
      ast_free_unattached(type);
      return false;
    }

    typearg = ast_sibling(typearg);
  }

  if(!expr_nominal(&nominal))
  {
    ast_error(ast, "couldn't create a type for 'this'");
    ast_free_unattached(type);
    return false;
  }

  return true;
}

bool expr_tuple(ast_t* ast)
{
  ast_t* child = ast_child(ast);
  ast_t* type;

  if(ast_sibling(child) == NULL)
  {
    type = ast_type(child);
  } else {
    type = ast_from(ast, TK_TUPLETYPE);

    while(child != NULL)
    {
      ast_t* c_type = ast_type(child);

      if(c_type == NULL)
      {
        ast_error(child,
          "a tuple can't contain a control flow statement that never results "
          "in a value");
        return false;
      }

      ast_append(type, c_type);
      child = ast_sibling(child);
    }
  }

  ast_settype(ast, type);
  ast_inheriterror(ast);
  return true;
}

bool expr_error(ast_t* ast)
{
  if(ast_sibling(ast) != NULL)
  {
    ast_error(ast, "error must be the last expression in a sequence");
    ast_error(ast_sibling(ast), "error is followed with this expression");
    return false;
  }

  ast_seterror(ast);
  return true;
}

bool expr_compiler_intrinsic(ast_t* ast)
{
  ast_t* fun = ast_enclosing_method_body(ast);
  ast_t* body = ast_childidx(fun, 6);
  ast_t* child = ast_child(body);

  if((child != ast) || (ast_sibling(child) != NULL))
  {
    ast_error(ast, "a compiler intrinsic must be the entire body");
    return false;
  }

  ast_settype(ast, ast_from(ast, TK_COMPILER_INTRINSIC));
  return true;
}

bool expr_nominal(ast_t** astp)
{
  // Resolve typealiases and typeparam references.
  if(!names_nominal(*astp, astp))
    return false;

  ast_t* ast = *astp;

  if(ast_id(ast) != TK_NOMINAL)
    return true;

  // If still nominal, check constraints.
  ast_t* def = (ast_t*)ast_data(ast);

  // Special case: don't check the constraint of a Pointer. This allows a
  // Pointer[Pointer[A]], which is normally not allowed, as a Pointer[A] is
  // not a subtype of Any.
  ast_t* id = ast_child(def);
  const char* name = ast_name(id);

  if(!strcmp(name, "Pointer"))
    return true;

  ast_t* typeparams = ast_childidx(def, 1);
  ast_t* typeargs = ast_childidx(ast, 2);

  return check_constraints(typeparams, typeargs);
}

bool expr_fun(ast_t* ast)
{
  ast_t* type = ast_childidx(ast, 4);
  ast_t* can_error = ast_sibling(type);
  ast_t* body = ast_sibling(can_error);

  if(ast_id(body) == TK_NONE)
    return true;

  ast_t* def = ast_enclosing_type(ast);
  bool is_trait = ast_id(def) == TK_TRAIT;

  // if specified, body type must match return type
  ast_t* body_type = ast_type(body);

  if(body_type == NULL)
  {
    ast_t* last = ast_childlast(body);
    ast_error(type, "function body always results in an error");
    ast_error(last, "function body expression is here");
    return false;
  }

  if(ast_id(body_type) == TK_COMPILER_INTRINSIC)
    return true;

  // check partial functions
  if(ast_id(can_error) == TK_QUESTION)
  {
    // if a partial function, check that we might actually error
    if(!is_trait && !ast_canerror(body))
    {
      ast_error(can_error, "function body is not partial but the function is");
      return false;
    }
  } else {
    // if not a partial function, check that we can't error
    if(ast_canerror(body))
    {
      ast_error(can_error, "function body is partial but the function is not");
      return false;
    }
  }

  if(ast_id(ast) == TK_FUN)
  {
    bool ok_sub = is_subtype(body_type, type);

    if(ok_sub)
      ok_sub = coerce_literals(body, type);

    if(!ok_sub)
    {
      ast_t* last = ast_childlast(body);
      ast_error(type, "function body isn't a subtype of the result type");
      ast_error(last, "function body expression is here");
      return false;
    }
  }

  return true;
}


static void propogate_coercion(ast_t* ast, ast_t* type)
{
  assert(ast != NULL);
  assert(type != NULL);

  if(!is_type_arith_literal(ast_type(ast)))
    return;

  ast_settype(ast, type);

  for(ast_t* p = ast_child(ast); p != NULL; p = ast_sibling(p))
    propogate_coercion(p, type);
}


bool is_type_arith_literal(ast_t* ast)
{
  assert(ast != NULL);
  token_id id = ast_id(ast);
  return (id == TK_INTLITERAL) || (id == TK_FLOATLITERAL);
}


static const char* const order[] =
{
  "U8", "I8", "U16", "I16", "U32", "I32", "U64", "I64",
  "U128", "I128", "F32", "F64", NULL
};


static size_t index_literal(ast_t* type)
{
  // Prefer non-nominal types.
  if(ast_id(type) != TK_NOMINAL)
    return -1;

  const char* name = ast_name(ast_childidx(type, 1));

  for(size_t i = 0; order[i] != NULL; i++)
  {
    if(!strcmp(order[i], name))
      return i;
  }

  // Shouldn't get here.
  assert(0);
  return -1;
}

static ast_t* wide_literal(ast_t* a, ast_t* b)
{
  if(a == NULL)
    return b;

  if(b == NULL)
    return a;

  size_t a_index = index_literal(a);
  size_t b_index = index_literal(b);

  return (a_index >= b_index) ? a : b;
}


static ast_t* narrow_literal(ast_t* a, ast_t* b)
{
  if(a == NULL)
    return b;

  if(b == NULL)
    return a;

  size_t a_index = index_literal(a);
  size_t b_index = index_literal(b);

  return (a_index <= b_index) ? a : b;
}


static bool nominal_check(ast_t* check, const char* category)
{
  ast_t* attempt = type_builtin(check, category);
  bool ok = is_subtype(check, attempt);
  ast_free_unattached(attempt);

  if(ok)
    ast_free_unattached(check);

  return ok;
}


static ast_t* nominal_literal(token_id literal_id, ast_t* type)
{
  ast_t* check = ast_dup(type);
  ast_t* cap = ast_childidx(check, 3);
  ast_setid(cap, TK_VAL);

  if(literal_id == TK_INTLITERAL)
  {
    if(nominal_check(check, "Signed"))
      return type;

    if(nominal_check(check, "Unsigned"))
      return type;
  }

  if(nominal_check(check, "Float"))
    return type;

  ast_free_unattached(check);
  return NULL;
}


static ast_t* structural_literal(token_id literal_id, ast_t* type)
{
  // Return the widest literal type that is a subtype of the structural type.
  ast_t* result = NULL;

  for(size_t i = 0; order[i] != NULL; i++)
  {
    ast_t* attempt = type_builtin(type, order[i]);
    attempt = nominal_literal(literal_id, attempt);

    if((attempt != NULL) && is_subtype(attempt, type))
      result = attempt;
  }

  return result;
}


static ast_t* union_literal(token_id literal_id, ast_t* type)
{
  // Return the widest type that the literal is a subtype of.
  ast_t* child = ast_child(type);
  ast_t* result = NULL;

  while(child != NULL)
  {
    ast_t* attempt = is_literal_subtype(literal_id, child);
    result = wide_literal(result, attempt);
    child = ast_sibling(child);
  }

  return result;
}


static ast_t* isect_literal(token_id literal_id, ast_t* type)
{
  // Return the narrowest type that the literal is a subtype of.
  ast_t* child = ast_child(type);
  ast_t* result = NULL;

  while(child != NULL)
  {
    ast_t* attempt = is_literal_subtype(literal_id, child);
    result = narrow_literal(result, attempt);
    child = ast_sibling(child);
  }

  return result;
}


static ast_t* arrow_literal(token_id literal_id, ast_t* type)
{
  // If the literal is a subtype of the right side, return the arrow
  // type. The literal will be assigned the arrow type, rather than the
  // right side.
  ast_t* right = ast_childidx(type, 1);
  ast_t* upper = viewpoint_upper(right);
  bool ok = is_literal_subtype(literal_id, upper) != NULL;
  ast_free_unattached(upper);

  if(ok)
    return type;

  return NULL;
}


static ast_t* constraint_literal(token_id literal_id, ast_t* type)
{
  if(ast_id(type) != TK_UNIONTYPE)
    return is_literal_subtype(literal_id, type);

  // Because this is an upper bounds, unions work differently here.
  // Every member of the union has to be a valid type for the literal.
  // Return the widest type that the literal is a subtype of.
  ast_t* child = ast_child(type);
  ast_t* result = NULL;

  while(child != NULL)
  {
    ast_t* attempt = constraint_literal(literal_id, child);

    if(attempt == NULL)
      return NULL;

    result = wide_literal(result, attempt);
    child = ast_sibling(child);
  }

  return result;
}


static ast_t* typeparam_literal(token_id literal_id, ast_t* type)
{
  // If the literal is a subtype of the constraint, return the type
  // parameter. The literal will be assigned the type parameter as a type,
  // rather than the constraint.
  ast_t* param = (ast_t*)ast_data(type);
  ast_t* constraint = ast_childidx(param, 1);

  if(constraint_literal(literal_id, constraint) != NULL)
    return type;

  return NULL;
}


ast_t* is_literal_subtype(token_id literal_id, ast_t* target)
{
  assert(literal_id == TK_INTLITERAL || literal_id == TK_FLOATLITERAL);
  assert(target != NULL);

  switch(ast_id(target))
  {
    case TK_NOMINAL:
      return nominal_literal(literal_id, target);

    case TK_STRUCTURAL:
      return structural_literal(literal_id, target);

    case TK_UNIONTYPE:
      return union_literal(literal_id, target);

    case TK_ISECTTYPE:
      return isect_literal(literal_id, target);

    case TK_TUPLETYPE:
      // A literal isn't a tuple.
      return false;

    case TK_ARROW:
      return arrow_literal(literal_id, target);

    case TK_TYPEPARAMREF:
      return typeparam_literal(literal_id, target);

    default: {}
  }

  assert(0);
  return false;
}


bool coerce_literals(ast_t* ast, ast_t* target_type)
{
  assert(ast != NULL);

  // TODO: handle local inference properly
  if(target_type == NULL)
    return true;

  if(ast_id(ast) == TK_TUPLE)
  {
    assert(ast_id(target_type) == TK_TUPLETYPE);

    ast_t* child = ast_child(ast);
    ast_t* target_child = ast_child(target_type);
    ast_t* new_type = ast_from(ast_type(ast), TK_TUPLETYPE);
    ast_t* last_sibling = NULL;

    while(child != NULL)
    {
      assert(target_child != NULL);
      assert(ast_id(child) == TK_SEQ);

      if(!coerce_literals(ast_child(child), target_child))
      {
        ast_free(new_type);
        return false;
      }

      ast_t* new_element_type = ast_dup(ast_type(ast_child(child)));
      if(last_sibling == NULL)
        ast_add(new_type, new_element_type);
      else
        ast_add_sibling(last_sibling, new_element_type);

      last_sibling = new_element_type;
      child = ast_sibling(child);
      target_child = ast_sibling(target_child);
    }

    assert(target_child == NULL);
    ast_settype(ast, new_type);
    return true;
  }

  ast_t* type = ast_type(ast);

  if(type == NULL || !is_type_arith_literal(type))
    return true;

  target_type = is_literal_subtype(ast_id(type), target_type);

  if(target_type == NULL)
  {
    ast_error(ast, "cannot determine type of literal");
    return false;
  }

  ast_t* prom_type = ast_dup(target_type);
  ast_t* cap = ast_childidx(prom_type, 3);

  if(cap != NULL)
    ast_setid(cap, TK_VAL);

  propogate_coercion(ast, prom_type);
  return true;
}
