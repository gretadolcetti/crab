/*******************************************************************************
 *
 * Anti-unification domain -- lifting a value domain using term
 * equivalences.
 *
 * Author: Graeme Gange (gkgange@unimelb.edu.au)
 ******************************************************************************/

#ifndef IKOS_ANTI_UNIF_HPP
#define IKOS_ANTI_UNIF_HPP

#include <utility>
#include <algorithm>
#include <iostream>
#include <vector>
#include <sstream>

// Uncomment for enabling debug information
// #include <ikos/common/dbg.hpp>

#include <ikos/common/types.hpp>
#include <ikos/common/bignums.hpp>
#include <ikos/cfg/VarFactory.hpp>
#include <ikos/algorithms/linear_constraints.hpp>
#include <ikos/domains/numerical_domains_api.hpp>
#include <ikos/domains/bitwise_operators_api.hpp>
#include <ikos/domains/division_operators_api.hpp>
#include <ikos/domains/intervals.hpp>
#include <ikos/domains/term/term_expr.hpp>
#include <ikos/domains/term/inverse.hpp>

#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/optional.hpp>

#define WARN(x) fprintf(stderr, "WARNING: %s\n", x)

using namespace boost;
using namespace std;

//#define VERBOSE 

namespace ikos {

  namespace term {
     template<class Num, class VName, class Abs>
     class TDomInfo {
      public:
       typedef Num Number;
       typedef VName VariableName;
       typedef cfg::var_factory_impl::StrVarAlloc_col Alloc;
       typedef Abs domain_t; 
     };
  }

  template< typename Info >
  class anti_unif: public writeable,
                 public numerical_domain<typename Info::Number, typename Info::VariableName >, 
                 public bitwise_operators< typename Info::Number, typename Info::VariableName >,
                 public division_operators< typename Info::Number, typename Info::VariableName > {
  private:
    // Underlying (value?) domain.
    typedef typename Info::Number Number;
    typedef typename Info::VariableName VariableName;
    typedef typename Info::domain_t  dom_t;

    typedef typename dom_t::variable_t dom_var_t;
    typedef typename Info::Alloc dom_var_alloc_t;
    typedef typename dom_var_alloc_t::varname_t dom_varname_t;
    typedef patricia_tree_set< dom_var_t > domvar_set_t;

    typedef bound<Number> bound_t;
    typedef interval<Number> interval_t;
    
   public:
    typedef variable< Number, VariableName >                 variable_t;
    typedef linear_constraint< Number, VariableName >        linear_constraint_t;
    typedef linear_constraint_system< Number, VariableName > linear_constraint_system_t;
    typedef linear_expression< Number, VariableName >        linear_expression_t;
    typedef anti_unif<Info>        anti_unif_t;

    typedef term::term_table< Number, operation_t > ttbl_t;
    typedef typename ttbl_t::term_id_t term_id_t;
    typedef patricia_tree_set< VariableName >  varname_set_t;
     
   private:
    // WARNING: assumes the underlying domain uses the same number type.
    typedef typename Info::Number                     dom_number;
    typedef typename dom_t::linear_constraint_t        dom_lincst_t;
    typedef typename dom_t::linear_constraint_system_t dom_linsys_t;
    typedef typename dom_t::linear_expression_t        dom_linexp_t;

    typedef typename linear_expression_t::component_t linterm_t;

    typedef container::flat_map< term_id_t, dom_var_t > term_map_t;
    typedef container::flat_map< dom_var_t, variable_t > rev_map_t;
    typedef container::flat_set< term_id_t > term_set_t;
    typedef container::flat_map< variable_t, term_id_t > var_map_t;

   private:
    bool     _is_bottom;
    // Uses a single state of the underlying domain.
    ttbl_t          _ttbl;
    dom_t           _impl;
    dom_var_alloc_t _alloc;
    var_map_t       _var_map;
    term_map_t      _term_map;
    term_set_t      changed_terms; 

    void set_to_bottom (){
      this->_is_bottom = true;
    }

   private:
    anti_unif(bool is_top): _is_bottom(!is_top) { }

    anti_unif(dom_var_alloc_t alloc, var_map_t vm, ttbl_t tbl, term_map_t tmap, dom_t impl)
        : _is_bottom((impl.is_bottom ())? true: false), _ttbl(tbl), _impl(impl), _alloc(alloc), 
          _var_map(vm), _term_map(tmap)
    { check_terms(); }

    // x = y op [lb,ub]
    term_id_t term_of_itv(bound_t lb, bound_t ub)
    {
      optional<Number> n_lb = lb.number ();
      optional<Number> n_ub = ub.number ();

      if (n_lb && n_ub && (*n_lb == *n_ub))
        return build_const(*n_lb);

      term_id_t t_itv = _ttbl.fresh_var();
      dom_var_t dom_itv = domvar_of_term(t_itv);
      _impl.set(dom_itv.name (), interval_t (lb, ub));
      return t_itv;
    }

    term_id_t term_of_expr(operation_t op, term_id_t ty, term_id_t tz)
    {
      optional<term_id_t> opt_tx = _ttbl.find_ftor(op, ty, tz);
      if(opt_tx)
      {
        // If the term already exists, we can learn nothing.
        return *opt_tx;
      } else {
        // Otherwise, assign the term, and evaluate.
        term_id_t tx = _ttbl.apply_ftor(op, ty, tz);
        _impl.apply(op,
            domvar_of_term(tx).name(),
            domvar_of_term(ty).name(), domvar_of_term(tz).name());
        return tx;
      }
    }


    void apply(operation_t op, VariableName x, VariableName y, bound_t lb, bound_t ub){	
      term_id_t t_x = term_of_expr(op, term_of_var(y), term_of_itv(lb, ub));
      // JNL: check with Graeme
      //      insert only adds an entry if the key does not exist
      //_var_map.insert(std::make_pair(x, t_x));
      rebind_var (x, t_x);

      check_terms();
    }

    // Apply a given functor in the underlying domain.
    void eval_ftor(dom_t& dom, ttbl_t& tbl, term_id_t t)
    {
      // Get the term info.
      typename ttbl_t::term_t* t_ptr = tbl.get_term_ptr(t); 

      // Only apply functors.
      if(t_ptr->kind() == term::TERM_APP)
      {
        operation_t op = term::term_ftor(t_ptr);
        
        std::vector<term_id_t>& args(term::term_args(t_ptr));
        assert(args.size() == 2);
        dom.apply(op, domvar_of_term(t).name(),
            domvar_of_term(args[0]).name(),
            domvar_of_term(args[1]).name());
      }
    }

    void eval_ftor_down(dom_t& dom, ttbl_t& tbl, term_id_t t)
    {
      // Get the term info.
      typename ttbl_t::term_t* t_ptr = tbl.get_term_ptr(t); 

      // Only apply functors.
      if(t_ptr->kind() == term::TERM_APP)
      {
        operation_t op = term::term_ftor(t_ptr);
        
        std::vector<term_id_t>& args(term::term_args(t_ptr));
        assert(args.size() == 2);
        term::InverseOps<dom_number, dom_varname_t, dom_t>::apply(dom, op,
            domvar_of_term(t).name(),
            domvar_of_term(args[0]).name(),
            domvar_of_term(args[1]).name());
      }
    }


    dom_t eval_ftor_copy(dom_t& dom, ttbl_t& tbl, term_id_t t)
    {
      // cout << "Before tightening:" << endl;
      // cout << dom << endl;
      dom_t ret = dom;
      eval_ftor(ret, tbl, t);
      // std::cout << "After tightening:" << endl << ret << endl;
      return ret;
    }  

  public:
    static anti_unif_t top() {
      return anti_unif(true);
    }
    
    static anti_unif_t bottom() {
      return anti_unif(false);
    }
    
  public:
    // Constructs top octagon, represented by a size of 0.
    anti_unif(): _is_bottom(false) { }
    
    anti_unif(const anti_unif_t& o): 
       writeable(), 
       numerical_domain<Number, VariableName >(),
       bitwise_operators< Number, VariableName >(),
       division_operators< Number, VariableName >(),   
       _is_bottom(o._is_bottom), 
       _ttbl(o._ttbl), _impl(o._impl),
       _alloc(o._alloc),
       _var_map(o._var_map), _term_map(o._term_map),
       changed_terms(o.changed_terms)
    { check_terms(); } 

    anti_unif_t operator=(anti_unif_t o) {
      o.check_terms();
      _is_bottom= o.is_bottom();
      _ttbl = o._ttbl;
      _impl = o._impl;
      _alloc = o._alloc;
      _var_map= o._var_map;
      _term_map= o._term_map;
      changed_terms = o.changed_terms;

      check_terms();
      return *this;
    }
       
    bool is_bottom() {
      return _is_bottom;
    }
    
    bool is_top() {
      return !_var_map.size() && !is_bottom();
    }
    
    bool is_normalized(){
      return changed_terms.size() == 0;
      // return _is_normalized(_impl);
    }

    varname_set_t get_variables() const {
      varname_set_t vars;
      for(auto& p : _var_map)
      {
        variable_t v(p.first);
        vars += v.name();
      }
      return vars;
    }

    // Lattice operations
    bool operator<=(anti_unif_t o)  {	
      // Require normalization of the first argument
      this->normalize();

      if (is_bottom()) {
        return true;
      } else if(o.is_bottom()) {
        return false;
      } else {
        typename ttbl_t::term_map_t gen_map;
        dom_var_alloc_t palloc(_alloc, o._alloc);

        // Build up the mapping of o onto this, variable by variable.
        // Assumption: the set of variables in x & o are common.
        for(auto p : _var_map)
        {
          if(!_ttbl.map_leq(o._ttbl, term_of_var(p.first), o.term_of_var(p.first), gen_map))
            return false;
        }
        // We now have a mapping of reachable y-terms to x-terms.
        // Create copies of _impl and o._impl with a common
        // variable set.
        dom_t x_impl(_impl);
        dom_t y_impl(o._impl);

        // Perform the mapping
        vector<dom_var_t> xvars; 
        vector<dom_var_t> yvars;
        xvars.reserve (gen_map.size ());
        yvars.reserve (gen_map.size ());
        for(auto p : gen_map)
        {
          // dom_var_t vt = _alloc.next();
          dom_var_t vt = palloc.next();
          dom_var_t vx = domvar_of_term(p.second); 
          dom_var_t vy = o.domvar_of_term(p.first);

          xvars.push_back (vx);
          yvars.push_back (vy);

          x_impl.assign(vt.name(), dom_linexp_t(vx));
          y_impl.assign(vt.name(), dom_linexp_t(vy));
        }
        for(auto vx : xvars) x_impl -= vx.name();
        for(auto vy : yvars) y_impl -= vy.name();

        return x_impl <= y_impl;
      }
    } 

    anti_unif_t operator|(anti_unif_t o) {
      // Requires normalization of both operands
      normalize();
      o.normalize();

      if (is_bottom() || o.is_top()) {
        return o;
      } 
      else if(o.is_bottom() || is_top ()) {
        return *this;
      }       
      else {
        // First, we need to compute the new term table.
        ttbl_t out_tbl;
        // Mapping of (term, term) pairs to terms in the join state
        typename ttbl_t::gener_map_t gener_map;
        
        var_map_t out_vmap;

        dom_var_alloc_t palloc(_alloc, o._alloc);

        // For each program variable in state, compute a generalization
        for(auto p : _var_map)
        {
          variable_t v(p.first);
          term_id_t tx(term_of_var(v));
          term_id_t ty(o.term_of_var(v));

          term_id_t tz = _ttbl.generalize(o._ttbl, tx, ty, out_tbl, gener_map);
          assert(tz < out_tbl.size());
          out_vmap[v] = tz;
        }

        // Rename the common terms together
        dom_t x_impl(_impl);
        dom_t y_impl(o._impl);

        // Perform the mapping
        term_map_t out_map;
        
        vector<dom_var_t> xvars; 
        vector<dom_var_t> yvars;
        xvars.reserve (gener_map.size ());
        yvars.reserve (gener_map.size ());
        for(auto p : gener_map)
        {
          auto txy = p.first;
          term_id_t tz = p.second;
          dom_var_t vt = palloc.next();
          out_map.insert(std::make_pair(tz, vt));

          dom_var_t vx = domvar_of_term(txy.first);
          dom_var_t vy = o.domvar_of_term(txy.second);

          xvars.push_back (vx);
          yvars.push_back (vy);

          x_impl.assign(vt.name(), dom_linexp_t(vx));
          y_impl.assign(vt.name(), dom_linexp_t(vy));
        }
        
        IKOS_DEBUG("============","JOIN","==================");
        // IKOS_DEBUG(*this,"\n","~~~~~~~~~~~~~~~~");
        // IKOS_DEBUG(o, "\n","----------------");
        // IKOS_DEBUG("x = ",_impl);
        // IKOS_DEBUG("y = ",o._impl);

        // IKOS_DEBUG("ren_0(x) = ", x_impl);
        // IKOS_DEBUG("ren_0(y) = ",  y_impl);

        // TODO: for relational domains we should use
        // domain_traits::forget which can be more efficient than
        // removing one by one.
        for(auto vx : xvars) x_impl -= vx.name();
        for(auto vy : yvars) y_impl -= vy.name();
        
        dom_t x_join_y = x_impl|y_impl;
        anti_unif_t res (anti_unif (palloc, out_vmap, out_tbl, out_map, x_join_y));

        //IKOS_DEBUG("After elimination:\n", res);
        return res;
      }
    }

    // Widening
    anti_unif_t operator||(anti_unif_t o) {
      // The left operand of the widenning cannot be closed, otherwise
      // termination is not ensured. However, if the right operand is
      // close precision may be improved.
      o.normalize();
      if (is_bottom()) {
        return o;
      } 
      else if(o.is_bottom()) {
        return *this;
      } 
      else {
        // First, we need to compute the new term table.
        ttbl_t out_tbl;
        // Mapping of (term, term) pairs to terms in the join state
        typename ttbl_t::gener_map_t gener_map;
        
        var_map_t out_vmap;

        dom_var_alloc_t palloc(_alloc, o._alloc);

        // For each program variable in state, compute a generalization
        for(auto p : _var_map)
        {
          variable_t v(p.first);
          term_id_t tx(term_of_var(v));
          term_id_t ty(o.term_of_var(v));

          term_id_t tz = _ttbl.generalize(o._ttbl, tx, ty, out_tbl, gener_map);
          out_vmap[v] = tz;
        }

        // Rename the common terms together
        dom_t x_impl(_impl);
        dom_t y_impl(o._impl);

        // Perform the mapping
        term_map_t out_map;
        vector<dom_var_t> xvars; 
        vector<dom_var_t> yvars;
        xvars.reserve (gener_map.size ());
        yvars.reserve (gener_map.size ());
        for(auto p : gener_map)
        {
          auto txy = p.first;
          term_id_t tz = p.second;
          dom_var_t vt = palloc.next();
          out_map.insert(std::make_pair(tz, vt));

          dom_var_t vx = domvar_of_term(txy.first);
          dom_var_t vy = o.domvar_of_term(txy.second);

          xvars.push_back(vx);
          yvars.push_back(vy);

          x_impl.assign(vt.name(), dom_linexp_t(vx));
          y_impl.assign(vt.name(), dom_linexp_t(vy));
        }
        for(auto vx : xvars) x_impl -= vx.name();
        for(auto vy : yvars) y_impl -= vy.name();

        dom_t x_widen_y = x_impl||y_impl;
        anti_unif_t res (palloc, out_vmap, out_tbl, out_map, x_widen_y);

        IKOS_DEBUG("============","WIDENING", "==================");
        // IKOS_DEBUG(x_impl,"\n~~~~~~~~~~~~~~~~");
        // IKOS_DEBUG(y_impl,"\n----------------");
        // IKOS_DEBUG(x_widen_y,"\n================");

        return res;
      }
    }

    // Meet
    anti_unif_t operator&(anti_unif_t o) {
      // Does not require normalization of any of the two operands
      if (is_bottom() || o.is_bottom()) {
        return bottom();
      } 
      else if (is_top()) {
        return o;
      }
      else if (o.is_top()) {
        return *this;
      }
      else {
        WARN ("ANTI-UNIF: meet not yet implemented.");
        // If meet is only used to refine instead of narrowing then we
        // should return the second argument.
        return o;
      }
    }
    
    // Narrowing
    anti_unif_t operator&&(anti_unif_t o) {	
      // Does not require normalization of any of the two operands
      if (is_bottom() || o.is_bottom()) {
        return bottom();
      } 
      else if (is_top ())
        return o;
      else {
        WARN ("ANTI-UNIF: narrowing not yet implemented.");
        return *this; 
      }
    } // Returned matrix is not normalized.

    void operator-=(VariableName v) {
      // Remove a variable from the scope
      auto it(_var_map.find(v));
      if(it != _var_map.end())
      {
        term_id_t t = (*it).second;
        _var_map.erase(it); 
        dom_var_t dom_v(domvar_of_term(t));
        _impl -= dom_v.name();
        _term_map.erase (t);
      }
    }

    void check_terms(void)
    {
      for(auto p : _var_map)
        assert(p.second < _ttbl.size());
    }
    template<class T>
    T check_terms(T& t)
    {
      check_terms();
      return t;
    }

    void rebind_var(variable_t& x, term_id_t tx)
    {
      auto it(_var_map.find(x));
      if(it != _var_map.end())
        _var_map.erase(it);
      _var_map.insert(std::make_pair(x, tx));
    }

    // Build the tree for a linexpr, and ensure that
    // values for the subterms are sustained.
    term_id_t build_const(const Number& n)
    {
      dom_number dom_n(n);
      optional<term_id_t> opt_n(_ttbl.find_const(dom_n));
      if(opt_n) {
        return *opt_n;
      } else {
        term_id_t term_n(_ttbl.make_const(dom_n));
        dom_var_t v = domvar_of_term(term_n);

        dom_linexp_t exp(n);
        _impl.assign(v.name(), exp);
        return term_n;
      }
    }

    term_id_t build_linterm(linterm_t term)
    {
      return build_term(OP_MULTIPLICATION,
                        build_const(term.first),
                        term_of_var(term.second));
    }

    term_id_t build_linexpr(linear_expression_t& e)
    {
      Number cst = e.constant();
      term_id_t t = build_const(cst);
      for(auto y: e) {
        t = build_term(OP_ADDITION, t, build_linterm(y));
      }
      IKOS_DEBUG("Should have ", domvar_of_term(t).name(), " := ", e,"\n",_impl);
      return t;       
    }

    term_id_t build_term(operation_t op, term_id_t ty, term_id_t tz)
    {
      // Check if the term already exists
      optional<term_id_t> eopt(_ttbl.find_ftor(op, ty, tz));
      if(eopt) {
        return *eopt;
      } else {
        // Create the term
        term_id_t tx = _ttbl.apply_ftor(op, ty, tz);
        dom_var_t v(domvar_of_term(tx));

        dom_var_t y(domvar_of_term(ty));
        dom_var_t z(domvar_of_term(tz));

        // Set up the evaluation.
        IKOS_DEBUG("Prev: ",_impl);

        _impl.apply(op, v.name(), y.name(), z.name());

        IKOS_DEBUG("Should have ", v.name(), "|", v.name().index()," := ", 
                    op,"(",y.name(), "|", y.name().index(), ",", z.name(), "|", z.name().index(), ")");
        IKOS_DEBUG(_impl);

        return tx;
      }
    }

    void assign(VariableName x_name, linear_expression_t e) {
      if (this->is_bottom()) {
        return;
      } else {

        //dom_linexp_t dom_e(rename_linear_expr(e));
        term_id_t tx(build_linexpr(e));
        variable_t x(x_name);
        rebind_var(x, tx);

        check_terms();

        IKOS_DEBUG("*** Assign ",x_name,":=", e,":", *this);
        return;
      }
    }

    //! copy of x into a new fresh variable y
    void expand (VariableName x_name, VariableName y_name) {
      if (is_bottom ()) {
        return;
      }
      else {
      
        variable_t x(x_name);
        variable_t y(y_name);
        linear_expression_t e(x);
        term_id_t tx(build_linexpr(e));
        rebind_var(y, tx);

        check_terms();
      }
    }


    // Apply operations to variables.

    // x = y op z
    void apply(operation_t op, VariableName x, VariableName y, VariableName z){	
      if (this->is_bottom()) {
        return;   
      } else {
        variable_t vx(x);
          
        term_id_t tx(build_term(op, term_of_var(y), term_of_var(z)));
        rebind_var(vx, tx);
      }
      check_terms();
      IKOS_DEBUG("*** Apply ", x, ":=", y, " ", op, " ", z, ":", *this);
    }
    
    // x = y op k
    void apply(operation_t op, VariableName x, VariableName y, Number k){	
      if (this->is_bottom()) {
        return;   
      } else {
        variable_t vx(x);
          
        term_id_t tx(build_term(op, term_of_var(y), build_const(k)));
        rebind_var(vx, tx);
      }
      check_terms();
      IKOS_DEBUG("*** Apply ", x, ":=", y, " ", op, " ", k, ":", *this);
      return;
    }

    term_id_t term_of_var(variable_t v)
    {
      auto it(_var_map.find(v)); 
      if(it != _var_map.end())
      {
        // assert ((*it).first == v);
        assert(_ttbl.size() > (*it).second);
        return (*it).second;
      } else {
        // Allocate a fresh term
        term_id_t id(_ttbl.fresh_var());
        _var_map[v] = id;
        return id;
      }
    }

    dom_var_t domvar_of_term(term_id_t id)
    {
      typename term_map_t::iterator it(_term_map.find(id));
      if(it != _term_map.end()) {
        return (*it).second;
      } else {
        // Allocate a fresh variable
        dom_var_t dvar(_alloc.next());
        _term_map.insert(std::make_pair(id, dvar));
        return dvar;
      }
    }

    dom_var_t domvar_of_var(variable_t v)
    {
      return domvar_of_term(term_of_var(v));
    }

    // Remap a linear constraint to the domain.
    dom_linexp_t rename_linear_expr(linear_expression_t exp)
    {
      Number cst(exp.constant());
      dom_linexp_t dom_exp(cst);
      for(auto v : exp.variables())
      {
        dom_exp = dom_exp + exp[v]*domvar_of_var(v);
      }
      return dom_exp;
    }

    dom_lincst_t rename_linear_cst(linear_constraint_t cst)
    {
      return dom_lincst_t(rename_linear_expr(cst.expression()), 
                          (typename dom_lincst_t::kind_t) cst.kind());
    }

    void operator+=(linear_constraint_t cst) {  
      dom_lincst_t cst_rn(rename_linear_cst(cst));
      _impl += cst_rn;
      
      // Possibly tightened some variable in cst
      for(auto v : cst.expression().variables())
      {
        changed_terms.insert(term_of_var(v));
      }
      // Probably doesn't need to done so eagerly.
      normalize();

      IKOS_DEBUG("*** Assume ",cst,":", *this);
      return;
    }

    // Assumption: vars(exp) subseteq keys(map)
    linear_expression_t rename_linear_expr_rev(dom_linexp_t exp, rev_map_t rev_map)
    {
      Number cst(exp.constant());
      linear_expression_t rev_exp(cst);
      for(auto v : exp.variables())
      {
        auto it = rev_map.find(v);
        assert(it != rev_map.end());
        variable_t v_out((*it).second);
        rev_exp = rev_exp + exp[v]*v_out;
      }

      return rev_exp;
    }

    linear_constraint_t rename_linear_cst_rev(dom_lincst_t cst, rev_map_t rev_map)
    {
      return linear_constraint_t(rename_linear_expr_rev(cst.expression(), rev_map),
          (typename linear_constraint_t::kind_t) cst.kind());
    }

    /*
    // If the children of t have changed, see if re-applying
    // the definition of t tightens the domain.
    void tighten_term(term_id_t t)
    {
      dom_t tight = _impl&eval_ftor_copy(_impl, _ttbl, t); 
      
      if(!(_impl <= tight))
      {
        // Applying the functor has changed the domain
        _impl = tight;
        for(term_id_t p : _ttbl.parents(t))
          tighten_term(p);
      }
      check_terms();
    }
    */
    
    void queue_push(vector< vector<term_id_t> >& queue, term_id_t t)
    {
      int d = _ttbl.depth(t);
      while(queue.size() <= d)
        queue.push_back(vector<term_id_t>());
      queue[d].push_back(t);
    }

    // Propagate information from tightened terms to
    // parents/children.
    void normalize(){

      // First propagate down, then up.   
      vector< vector< term_id_t > > queue;

      for(term_id_t t : changed_terms)
      {
        queue_push(queue, t);
      }

      dom_t d_prime = _impl;
      // Propagate information to children.
      // Don't need to propagate level 0, since
      //
      for(int d = queue.size()-1; d > 0; d--)
      {
        for(term_id_t t : queue[d])
        {
          eval_ftor_down(d_prime, _ttbl, t);
          if(!(_impl <= d_prime))
          {
            _impl = d_prime;
            // Enqueue the args
            typename ttbl_t::term_t* t_ptr = _ttbl.get_term_ptr(t); 
            std::vector<term_id_t>& args(term::term_args(t_ptr));
            for(term_id_t c : args)
            {
              if(changed_terms.find(c) == changed_terms.end())
              {
                changed_terms.insert(c);
                queue[_ttbl.depth(c)].push_back(c);
              }
            }
          }
        }
      }

      // Collect the parents of changed terms.
      term_set_t up_terms;
      vector< vector<term_id_t> > up_queue;
      for(term_id_t t : changed_terms)
      {
        for(term_id_t p : _ttbl.parents(t))
        {
          if(up_terms.find(p) == up_terms.end())
          {
            up_terms.insert(p);
            queue_push(up_queue, p);
          }
        }
      }

      // Now propagate up, level by level.
      assert(up_queue.size() == 0 || up_queue[0].size() == 0);
      for(int d = 1; d < up_queue.size(); d++)
      {
        // up_queue[d] shouldn't change.
        for(term_id_t t : up_queue[d])
        {
          eval_ftor(d_prime, _ttbl, t);
          if(!(_impl <= d_prime))
          {
            _impl = d_prime;
            for(term_id_t p : _ttbl.parents(t))
            {
              if(up_terms.find(p) == up_terms.end())
              {
                up_terms.insert(p);
                queue_push(up_queue, p);
              }
            }
          }
        }
      }

      changed_terms.clear();

      if (_impl.is_bottom ())
        set_to_bottom ();

    }

    interval_t operator[](VariableName x) { 
      // Needed for accuracy
      normalize();

      if (is_bottom ()) return interval_t::bottom ();

      variable_t vx (x);
      auto it = _var_map.find (vx);
      if (it == _var_map.end ()) 
        return interval_t::top ();
      
      dom_var_t dom_x = domvar_of_term(it->second);

      return _impl[dom_x];
    } 

    void set (VariableName x, interval_t intv){
      variable_t vx (x);
      rebind_var (vx, term_of_itv (intv.lb (), intv.ub ()));
    }
    
    void operator+=(linear_constraint_system_t cst) {
      for(typename linear_constraint_system_t::iterator it=cst.begin(); it!= cst.end(); ++it){
        this->operator+=(*it);
      }
    }
    
    linear_constraint_system_t to_linear_constraint_system(void)
    {
      // Extract the underlying constraint system
      // dom_linsys_t dom_sys(_impl.to_linear_constraint_system());

      // Collect the visible terms
      rev_map_t rev_map;
      std::vector< std::pair<variable_t, variable_t> > equivs;
      for(auto p : _var_map)
      {
        dom_var_t dv = domvar_of_term(p.second);

        auto it = rev_map.find(dv);
        if(it == rev_map.end()){
          // The term has not yet been seen.
          rev_map.insert(std::make_pair(dv, p.first));
        } else {
          // The term is already mapped to (*it).second,
          // so add an equivalence.
          equivs.push_back(std::make_pair((*it).second, p.first)); 
        }
      }

      // Create a copy of _impl with only visible variables.
      dom_t d_vis(_impl);
      for(auto p : _term_map)
      {
        dom_var_t dv = p.second;
        if(rev_map.find(dv) == rev_map.end())
          d_vis -= dv.name();
      }


      // Now build and rename the constraint system, plus equivalences.
      dom_linsys_t dom_sys(d_vis.to_linear_constraint_system());

      linear_constraint_system_t out_sys; 
      for(dom_lincst_t cst : dom_sys) {

        // JNL:cst can have variables that are not in rev_map (e.g.,
        // some generated by build_linexpr). If the constraint
        // contains one then we ignore the constraint.
        bool is_rev_mapped = true;
        for(auto v : cst.variables()) {
          auto it = rev_map.find(v);
          if (it == rev_map.end())
            is_rev_mapped = false;
        }

        if (is_rev_mapped)
          out_sys += rename_linear_cst_rev(cst, rev_map);
      }
      
      for(auto p : equivs) {
        IKOS_DEBUG("Added equivalence ", p.first, "=", p.second);
        out_sys += (p.first - p.second == 0);
      }

      // Now rename it back into the external scope.
      return out_sys;
    }


    void apply(conv_operation_t op, VariableName x, VariableName y, unsigned width){
      // since reasoning about infinite precision we simply assign and
      // ignore the width.
      assign(x, linear_expression_t(y));
    }

    void apply(conv_operation_t op, VariableName x, Number k, unsigned width){
      // since reasoning about infinite precision we simply assign
      // and ignore the width.
      assign(x, k);
    }

    void apply(bitwise_operation_t op, VariableName x, VariableName y, VariableName z){
      // Convert to intervals and perform the operation
      WARN("bitwise operators not yet supported by term domain");

      term_id_t term_x = _ttbl.fresh_var();
      dom_var_t dvar_x = domvar_of_term(term_x);
      _impl.apply(op, dvar_x.name(), domvar_of_var(y).name(), domvar_of_var(z).name());
      // JNL: check with Graeme
      //      insert only adds an entry if the key does not exist
      //_var_map[x] = term_x;
      variable_t x_copy (x);
      rebind_var (x_copy, term_x);
    }
    
    void apply(bitwise_operation_t op, VariableName x, VariableName y, Number k){
      WARN("bitwise operators not yet supported by term domain");

      term_id_t term_x = _ttbl.fresh_var();
      dom_var_t dvar_x = domvar_of_term(term_x);
      _impl.apply(op, dvar_x.name(), domvar_of_var(y).name(), k);
      // JNL: check with Graeme
      //      insert only adds an entry if the key does not exist
      //_var_map[x] = term_x;
      variable_t x_copy (x);
      rebind_var (x_copy, term_x);
    }
    
    // division_operators_api
    
    void apply(div_operation_t op, VariableName x, VariableName y, VariableName z){
      WARN("div operators not yet supported by term domain");

      term_id_t term_x = _ttbl.fresh_var();
      dom_var_t dvar_x = domvar_of_term(term_x);
      _impl.apply(op, dvar_x.name(), domvar_of_var(y).name(), domvar_of_var(z).name());
      // JNL: check with Graeme
      //      insert only adds an entry if the key does not exist
      //_var_map[x] = term_x;
      variable_t x_copy (x);
      rebind_var (x_copy, term_x);
    }

    void apply(div_operation_t op, VariableName x, VariableName y, Number k){
      WARN("div operators not yet supported by term domain");

      term_id_t term_x = _ttbl.fresh_var();
      dom_var_t dvar_x = domvar_of_term(term_x);
      _impl.apply(op, dvar_x.name(), domvar_of_var(y).name(), k);
      // JNL: check with Graeme
      //      insert only adds an entry if the key does not exist
      //_var_map[x] = term_x;
      variable_t x_copy (x);
      rebind_var (x_copy, term_x);
    }
    
    // Output function
    ostream& write(ostream& o) { 
      // Normalization is not enforced in order to maintain accuracy
      // but we force it to display all the relationships.
      normalize();

      if(is_bottom ()){
        return o << "_|_";
      }
      if(_var_map.empty ()) {
        return o << "{}";
      }      

      bool first = true;
      o << "{" ;
      for(auto p : _var_map)
      {
        if(first)
          first = false;
        else
          o << ", ";
        o << p.first << " -> t" << p.second
          << "[" << domvar_of_term(p.second).name() << "]";
      }
      o << "}";
     
      // print underlying domain
      o << _impl;

#ifdef VERBOSE
      /// For debugging purposes     
      o << " ttbl={" << _ttbl << "}\n";
#endif 
      return o;
    }

    const char* getDomainName () const { 
      std::stringstream buf;
      buf << "term(" << _impl.getDomainName () << ")";
      std::string name(buf.str());
      return name.c_str ();
    }

  }; // class anti_unif

namespace domain_traits {

  template <typename Info, typename VariableName>
  void expand (anti_unif<Info>& inv, VariableName x, VariableName new_x) {
    inv.expand (x, new_x);
  }

  template <typename Info>
  void normalize (anti_unif<Info>& inv) {
    inv.normalize();
  }

} // namespace domain_traits

} // namespace ikos


#endif // IKOS_ANTI_UNIF_HPP
