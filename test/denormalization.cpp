#include <percy/percy.hpp>
#include <kitty/kitty.hpp>
#include <cassert>
#include <cstdio>
#include <fstream>

using namespace percy;
using kitty::static_truth_table;

/*******************************************************************************
    Tests denormalization of Boolean chains. This ensures that no chain output
    is inverted, but may result in suboptimal chains.
*******************************************************************************/
int main(void)
{
    
    {
        synth_spec<static_truth_table<2>> spec(2, 2);
        spec.verbosity = 0;

        auto synth = new_std_synth();
        chain<2> c;

        static_truth_table<2> in1, in2, f1, f2;

        kitty::create_nth_var(in1, 0);
        kitty::create_nth_var(in2, 1);

        f1 = in1 | in2;
        f2 = in1 & in2;

        spec.functions[0] = &f1;
        spec.functions[1] = &f2;

        auto result = synth->synthesize(spec, c);
        assert(result == success);

        // For one OR gate and one AND gate we need exactly 2 steps.
        assert(c.get_nr_vertices() == 2);
        auto tts = chain_simulate(c, spec);
        assert(tts[0] == f1);
        assert(tts[1] == f2);

        // After normalization we should still have 2 steps, since we have no
        // inverted outputs.
        c.denormalize();
        assert(c.get_nr_vertices() == 2);
        tts = chain_simulate(c, spec);
        assert(tts[0] == f1);
        assert(tts[1] == f2);

        // Let us now synthesize an OR and a NOR function. These can be shared
        // in a normalized chain.
        f2 = ~f1;
        result = synth->synthesize(spec, c);
        assert(result == success);
        assert(c.get_nr_vertices() == 1);
        tts = chain_simulate(c, spec);
        assert(tts[0] == f1);
        assert(tts[1] == f2);

        // Now when we denormalize we should have 2 steps, since we the
        // inverted output should have been removed.
        c.denormalize();
        assert(c.get_nr_vertices() == 2);
        tts = chain_simulate(c, spec);
        assert(tts[0] == f1);
        assert(tts[1] == f2);

        // Finally, suppose we have 3 outputs, where two are inverted but share
        // the same function. In this case we can still denormalize the chain,
        // but we only have to add one additional step instead of two.
        spec.set_nr_out(3);
        spec.functions[0] = &f1;
        spec.functions[1] = &f2;
        spec.functions[2] = &f2;

        result = synth->synthesize(spec, c);
        assert(result == success);
        assert(c.get_nr_vertices() == 1);
        tts = chain_simulate(c, spec);
        assert(tts[0] == f1);
        assert(tts[1] == f2);
        assert(tts[2] == f2);

        c.denormalize();
        assert(c.get_nr_vertices() == 2);
        tts = chain_simulate(c, spec);
        assert(tts[0] == f1);
        assert(tts[1] == f2);
        assert(tts[2] == f2);
    }

          
    return 0;
}

