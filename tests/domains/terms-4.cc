#include "../program_options.hpp"
#include "../common.hpp"

using namespace std;
using namespace crab::analyzer;
using namespace crab::cfg;
using namespace crab::cfg_impl;
using namespace crab::domain_impl;

int main (int argc, char** argv )
{
  SET_TEST_OPTIONS(argc,argv)

  variable_factory_t vfac;
  typedef interval< z_number> interval_t;
  
  z_var x(vfac["x"], crab::INT_TYPE);
  z_var y(vfac["y"], crab::INT_TYPE);
  z_var w(vfac["w"], crab::INT_TYPE);
  z_var z(vfac["z"], crab::INT_TYPE);

  {

    z_term_domain_t dom_left = z_term_domain_t::top ();
    z_term_domain_t dom_right = z_term_domain_t::top ();

    // ({w = a0, x = a0, y = '+' (a0,a1), z = a1}, { x=5, w=5, z=3, y=8 })
    dom_left.assign (x, 5);
    dom_left.assign (w, x);
    dom_left.assign (z, 3);
    dom_left.apply(OP_ADDITION, y, x, z);
    
    // ({w = b0,  x = '+' (b0,b1), y = b0, z = b1}, {y=8, w=8,z=2,x=10 })
    dom_right.assign (y, 8); 
    dom_right.assign (w, y);
    dom_right.assign (z, 2); 
    dom_right.apply(OP_ADDITION, x, w, z);

    // meet = ({x=y=w= '+' (c0,c1), z=c2},{_|_}) = _|_
    crab::outs() << "Meet" << "\n" << dom_left << " \n " << dom_right << "\n"; 
    z_term_domain_t l_meet_r = dom_left & dom_right;
    crab::outs() << "Result=" << l_meet_r << "\n";
  }

  {
    z_term_domain_t dom_left = z_term_domain_t::top ();
    z_term_domain_t dom_right = z_term_domain_t::top ();

    // ({w = a0, x = a0, y = '+' (a0,a1), z = a1}, {x=[5,8],w=[5,8],z=[1,10],y=[6,18]})
    dom_left.set (x, interval_t (5,8));
    dom_left.assign (w, x);
    dom_left.set (z, interval_t (1,10));
    dom_left.apply(OP_ADDITION, y, x, z);
    
    // ({w = b0,  x = '+' (b0,b1), y = b0, z = b1}, {y=[2,7],w=[2,7],z=[3,5],x=[5,12]})
    dom_right.set (y, interval_t (2,7)); 
    dom_right.assign (w, y);
    dom_right.set (z, interval_t (3,5)); 
    dom_right.apply(OP_ADDITION, x, w, z);

    // meet = ({x=y=w= '+' (c0,c1), z=c2},{x=[5,8], y=[6,7], z=[3,5], w=[5,7]})
    crab::outs() << "Meet" << "\n" << dom_left << " \n " << dom_right << "\n"; 
    z_term_domain_t l_meet_r = dom_left & dom_right;
    crab::outs() << "Result=" << l_meet_r << "\n";
  }

  {
    z_term_domain_t dom = z_term_domain_t::top ();
    z_var zero(vfac["v0"], crab::INT_TYPE);
    z_var one(vfac["v1"], crab::INT_TYPE);

    dom.set (zero, interval_t (0,0));
    dom.set (one, interval_t (1,1));

    dom.apply(OP_ADDITION, x, one, zero);
    dom.apply(OP_ADDITION, y, zero, one);
    z_lin_cst_t c1 (z_lin_t(x) == z_lin_t(y));
    crab::outs() << "Added " << c1 << "\n";
    crab::outs() << dom << "\n";
    dom += c1;
    crab::outs() << "Result=" << dom << "\n";
    z_lin_cst_t c2 (z_lin_t(x) != z_lin_t(y));
    crab::outs() << "Added " << c2 << "\n";
    dom += c2;
    crab::outs() << "Result=" << dom << "\n";
  }

  return 0;
}
