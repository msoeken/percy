#pragma once

#include "encoder_base.hpp"

namespace percy
{
    using abc::Abc_Var2Lit;

    template<int FI=2, typename Solver=sat_solver*>
    class fence_encoder
    {
        using fanin = typename dag<FI>::fanin;

        private:
            Solver* solver;

            int level_dist[65]; // How many steps are below a certain level
            int nr_levels; // The number of levels in the Boolean fence
            int nr_op_vars_per_step;
            int nr_op_vars;
            int nr_out_vars;
            int nr_sim_vars;
            int nr_sel_vars;
            int sel_offset;
            int ops_offset;
            int out_offset;
            int sim_offset;
            int fence_offset;

            abc::Vec_Int_t* vLits; // Dynamic vector of literals
            std::vector<std::array<fanin, FI>> svar_map;
            std::vector<int> nr_svar_map;


        public:
            fence_encoder()
            {
                // TODO: compute better upper bound on number of literals
                vLits = abc::Vec_IntAlloc(128);
            }

            ~fence_encoder()
            {
                abc::Vec_IntFree(vLits);
            }

            void
            set_solver(Solver* s)
            {
                solver = s;
            }

            template<typename TT>
            void 
            update_level_map(const synth_spec<TT>& spec, const fence& f)
            {
                nr_levels = f.nr_levels();
                level_dist[0] = spec.get_nr_in();
                for (int i = 1; i <= nr_levels; i++) {
                    level_dist[i] = level_dist[i-1] + f.at(i-1);
                }
            }

            /*******************************************************************
                Returns the level (in the Boolean chain) of the specified step.
            *******************************************************************/
            template<typename TT>
            int 
            get_level(const synth_spec<TT>& spec, int step_idx) const
            {
                // PIs are considered to be on level zero.
                if (step_idx < spec.get_nr_in()) {
                    return 0;
                } else if (step_idx == spec.get_nr_in()) { 
                    // First step is always on level one
                    return 1;
                }
                for (int i = 0; i <= nr_levels; i++) {
                    if (level_dist[i] > step_idx) {
                        return i;
                    }
                }
                return -1;
            }

            /*******************************************************************
                Returns the index of the first step on the specified level.
            *******************************************************************/
            int 
            first_step_on_level(int level) const
            {
                if (level == 0) { return 0; }
                return level_dist[level-1];
            }

            int 
            last_step_on_level(int level) const
            {
                return first_step_on_level(level + 1) - 1;
            }

            template<typename TT>
            int
            get_op_var(const synth_spec<TT>& spec, int step_idx, int var_idx) const
            {
                assert(step_idx < spec.nr_steps);
                assert(var_idx > 0);
                assert(var_idx <= nr_op_vars_per_step);

                return ops_offset + step_idx * nr_op_vars_per_step + var_idx-1;
            }

            int
            get_sel_var(int var_idx) const
            {
                assert(var_idx < nr_sel_vars);

                return sel_offset + var_idx;
            }

            template<typename TT>
            int
            get_sim_var(const synth_spec<TT>& spec, int step_idx, int t) const
            {
                assert(step_idx < spec.nr_steps);
                assert(t < spec.get_tt_size());

                return sim_offset + spec.get_tt_size() * step_idx + t;
            }

            template<typename TT>
            int 
            get_out_var(const synth_spec<TT>& spec, int h, int i) const
            {
                assert(h < spec.nr_nontriv);
                assert(i < spec.nr_steps);

                return out_offset + spec.nr_steps * h + i;
            }

            template<typename TT>
            void 
            create_variables(const synth_spec<TT>& spec)
            {
                std::array<int, FI> fanins;

                nr_op_vars_per_step = ((1u << FI) - 1);
                nr_op_vars = spec.nr_steps * nr_op_vars_per_step;
                nr_out_vars = spec.nr_nontriv * spec.nr_steps;
                nr_sim_vars = spec.nr_steps * spec.get_tt_size();

                // Ensure that steps are constrained to the proper level.
                nr_sel_vars = 0;
                svar_map.clear();
                nr_svar_map.resize(spec.nr_steps);
                for (int i = 0; i < spec.nr_steps; i++) {
                    // Determine the level of this step.
                    auto level = get_level(spec, i + spec.get_nr_in());
                    auto nr_svars_for_i = 0;
                    assert(level > 0);
                    for (auto z = first_step_on_level(level-1); 
                            z < first_step_on_level(level); z++) {
                        fanin_init<FI>(fanins, FI-1);
                        do  {
                            svar_map.push_back(fanins);
                            nr_svars_for_i++;
                        } while (fanin_inc<FI>(fanins, z));
                    }
                    nr_sel_vars += nr_svars_for_i;
                    nr_svar_map[i] = nr_svars_for_i;
                }

                sel_offset = 0;
                ops_offset = nr_sel_vars;
                out_offset = nr_sel_vars + nr_op_vars;
                sim_offset = nr_sel_vars + nr_op_vars + nr_out_vars;
                fence_offset = nr_sel_vars + nr_op_vars + nr_out_vars +
                    nr_sim_vars;

                solver_set_nr_vars(*solver, nr_op_vars + nr_out_vars +
                        nr_sim_vars + nr_sel_vars);
            }

            template<typename TT>
            bool
            create_main_clauses(const synth_spec<TT>& spec)
            {
                for (int t = 0; t < spec.get_tt_size(); t++) {
                    if (!create_tt_clauses(spec, t)) {
                        return false;
                    }
                }
                return true;
            }

            template<typename TT>
            void
            create_output_clauses(const synth_spec<TT>& spec)
            {
                // Every output points to an operand.
                if (spec.nr_nontriv > 1) {
                    for (int h = 0; h < spec.nr_nontriv; h++) {
                        for (int i = 0; i < spec.nr_steps; i++) {
                            abc::Vec_IntSetEntry(vLits, i, 
                                    Abc_Var2Lit(get_out_var(spec, h, i), 0));
                            if (spec.verbosity) {
                                printf("  output %d may point to step %d\n", 
                                        h+1, spec.get_nr_in()+i+1);
                            }
                        }
                        solver_add_clause(*solver,abc::Vec_IntArray(vLits), 
                                abc::Vec_IntArray(vLits) + spec.nr_steps);
                    }
                }

                // At least one of the outputs has to refer to the final
                // operator, otherwise it may as well not be there.
                const auto last_op = spec.nr_steps - 1;
                for (int h = 0; h < spec.nr_nontriv; h++) {
                    abc::Vec_IntSetEntry(vLits, h, 
                            Abc_Var2Lit(get_out_var(spec, h, last_op), 0));
                }
                solver_add_clause(*solver, abc::Vec_IntArray(vLits), 
                        abc::Vec_IntArray(vLits) + spec.nr_nontriv);
            }

            /*******************************************************************
                Add clauses that prevent trivial variable projection and
                constant operators from being synthesized.
            *******************************************************************/
            template<typename TT>
            void 
            create_nontriv_clauses(const synth_spec<TT>& spec)
            {
                static_truth_table<FI> triv_op;

                for (int i = 0; i < spec.nr_steps; i++) {
                    kitty::clear(triv_op);
                    
                    // Dissallow the constant zero operator.
                    for (int j = 1; j <= nr_op_vars_per_step; j++) {
                        abc::Vec_IntSetEntry(vLits, j-1,
                                Abc_Var2Lit(get_op_var(spec, i, j), 0));
                    }
                    solver_add_clause(*solver, abc::Vec_IntArray(vLits),
                            abc::Vec_IntArray(vLits) + nr_op_vars_per_step);
                    
                    // Dissallow all variable projection operators.
                    for (int n = 0; n < FI; n++) {
                        kitty::create_nth_var(triv_op, n);
                        for (int j = 1; j <= nr_op_vars_per_step; j++) {
                            abc::Vec_IntSetEntry(vLits, j-1,
                                    abc::Abc_Var2Lit(get_op_var(spec, i, j), 
                                        kitty::get_bit(triv_op, j)));
                        }
                        solver_add_clause(*solver, abc::Vec_IntArray(vLits),
                                abc::Vec_IntArray(vLits) + nr_op_vars_per_step);
                    }
                }
            }

            template<typename TT>
            bool 
            create_tt_clauses(const synth_spec<TT>& spec, int t)
            {
                auto ret = true;
                std::bitset<FI> fanin_asgn;
                int pLits[2];

                int svar_offset = 0;
                for (int i = 0; i < spec.nr_steps; i++) {
                    auto level = get_level(spec, i + spec.get_nr_in());
                    assert(level > 0);
                    
                    auto ctr = 0;
                    const auto nr_svars_for_i = nr_svar_map[i];

                    for (int j = 0; j < nr_svars_for_i; j++) {
                        const auto svar = j + svar_offset;
                        const auto& fanins = svar_map[svar];
                        // First add clauses 
                        // operator i computes zero.
                        int opvar_idx = 0;
                        clear_assignment<FI>(fanin_asgn);
                        while (true) {
                            next_assignment<FI>(fanin_asgn);
                            if (is_zero<FI>(fanin_asgn)) {
                                break;
                            }
                            opvar_idx++;
                            ret &= add_simulation_clause(spec, t, i, svar, 0,
                                    opvar_idx, fanins, fanin_asgn);
                        }

                        // Next, all cases where operator i computes one.
                        opvar_idx = 0;
                        ret &= add_simulation_clause(spec, t, i, svar, 1,
                                opvar_idx, fanins, fanin_asgn);
                        while (true) {
                            next_assignment<FI>(fanin_asgn);
                            if (is_zero<FI>(fanin_asgn)) {
                                break;
                            }
                            opvar_idx++;
                            ret &= add_simulation_clause(spec, t, i, svar, 1,
                                    opvar_idx, fanins, fanin_asgn);
                        }
                    }
                    svar_offset += nr_svars_for_i;

                    // If an output has selected this particular operand, we
                    // need to ensure that this operand's truth table satisfies
                    // the specified output function.
                    for (int h = 0; h < spec.nr_nontriv; h++) {
                        auto outbit = kitty::get_bit(
                                *spec.functions[spec.synth_functions[h]], t+1);
                        if ((spec.out_inv >> spec.synth_functions[h]) & 1) {
                            outbit = 1 - outbit;
                        }
                        pLits[0] = abc::Abc_Var2Lit(get_out_var(spec, h, i), 1);
                        pLits[1] = abc::Abc_Var2Lit(get_sim_var(spec, i, t), 
                                1 - outbit);
                        ret &= solver_add_clause(*solver, pLits, pLits+2);
                        if (spec.verbosity > 1) {
                            printf("  (g_%d_%d --> %sx_%d_%d)\n", h+1, 
                                    spec.get_nr_in()+i+1, 
                                    (1 - outbit) ?  "!" : "",
                                    spec.get_nr_in()+i+1, t+1);
                        }
                    }
                }

                return ret;
            }

            /*******************************************************************
                Ensure that each gate has FI operands.
            *******************************************************************/
            template<typename TT>
            void
            create_op_clauses(const synth_spec<TT>& spec)
            {
                auto svar_offset = 0;
                for (int i = 0; i < spec.nr_steps; i++) {
                    auto level = get_level(spec, i + spec.get_nr_in());
                    assert(level > 0);

                    const auto nr_svars_for_i = nr_svar_map[i];
                    
                    for (int j = 0; j < nr_svars_for_i; j++) {
                        abc::Vec_IntSetEntry(vLits, j,
                                abc::Abc_Var2Lit(get_sel_var(j + svar_offset),
                                    0));
                    }

                    solver_add_clause(*solver, abc::Vec_IntArray(vLits), 
                            abc::Vec_IntArray(vLits) + nr_svars_for_i);

                    svar_offset += nr_svars_for_i;
                }
            }

            /*******************************************************************
              Add clauses which ensure that every step is used at least once.
            *******************************************************************/
            template<typename TT>
            void 
            create_alonce_clauses(const synth_spec<TT>& spec)
            {
                for (int i = 0; i < spec.nr_steps; i++) {
                    auto ctr = 0;
                    for (int h = 0; h < spec.nr_nontriv; h++) {
                        abc::Vec_IntSetEntry(vLits, ctr++, 
                                abc::Abc_Var2Lit(get_out_var(spec, h, i), 0));
                    }

                    const auto level = get_level(spec, i + spec.get_nr_in());
                    const auto idx = spec.get_nr_in()+i;
                    for (int ip = i + 1; ip < spec.nr_steps; ip++) {
                        auto svar_offset = 0;
                        for (auto svi = 0; svi < ip; svi++) {
                            svar_offset += nr_svar_map[svi];
                        }

                        auto levelp = get_level(spec, ip + spec.get_nr_in());
                        assert(levelp >= level);
                        if (levelp == level) {
                            continue;
                        }

                        const auto nr_svars_for_ip = nr_svar_map[ip];
                        for (int j = 0; j < nr_svars_for_ip; j++) {
                            const auto svar = j + svar_offset;
                            const auto& fanins = svar_map[svar];
                            for (const auto fanin : fanins) {
                                if (fanin == idx) {
                                    abc::Vec_IntSetEntry(
                                            vLits, 
                                            ctr++, 
                                            Abc_Var2Lit(svar, 0));
                                    break;
                                }
                            }
                        }

                    }
                    solver_add_clause(
                            *solver, 
                            abc::Vec_IntArray(vLits),
                            abc::Vec_IntArray(vLits) + ctr);
                }
            }

            template<typename TT>
            bool 
            add_simulation_clause(
                    const synth_spec<TT>& spec, 
                    const int t, 
                    const int i, 
                    const int svar, 
                    const int output, 
                    const int opvar_idx,
                    const std::array<fanin, FI>& fanins,
                    const std::bitset<FI>& fanin_asgn)
            {
                int ctr = 0;

                for (int j = 0; j < FI; j++) {
                    auto child = fanins[j];
                    auto assign = fanin_asgn[j];
                    if (child < spec.get_nr_in()) {
                        if ((((t + 1) & (1 << child) ) ? 1 : 0 ) != assign) {
                            return true;
                        }
                    } else {
                        abc::Vec_IntSetEntry(vLits, ctr++, abc::Abc_Var2Lit(
                                    get_sim_var(spec, child - spec.get_nr_in(),
                                        t), assign));
                    }
                }

                abc::Vec_IntSetEntry(vLits, ctr++,
                        abc::Abc_Var2Lit(get_sel_var(svar), 1));
                abc::Vec_IntSetEntry(vLits, ctr++,
                        abc::Abc_Var2Lit(get_sim_var(spec, i, t), output));

                if (opvar_idx > 0) {
                    abc::Vec_IntSetEntry(vLits, ctr++, abc::Abc_Var2Lit(
                                get_op_var(spec, i, opvar_idx), 1 - output));
                }

                return solver_add_clause(*solver, abc::Vec_IntArray(vLits),
                        abc::Vec_IntArray(vLits) + ctr); 
            }

/*
            template<typename TT>
            void 
            create_noreapply_clauses(const synth_spec<TT>& spec)
            {
                int pLits[2];

                for (int i = 0; i < spec.nr_steps; i++) {
                    const auto level = get_level(spec, i + spec.get_nr_in());
                    const auto idx = spec.get_nr_in()+i;

                    for (int ip = i+1; ip < spec.nr_steps; ip++) {
                        const auto levelp = get_level(spec, ip +
                                spec.get_nr_in());
                        if (levelp == level) { 
                            // A node cannot have a node on the same level in
                            // its fanin.
                            continue;
                        }

                        for (auto k = first_step_on_level(level-1); 
                                k < first_step_on_level(level); k++) {
                            for (int j = 0; j < k; j++) {
                                pLits[0] = abc::Abc_Var2Lit(
                                        spec.selection_vars[i][j][k], 1);

                                // Note that it's possible for node ip to never have
                                // i as its second fanin.
                                for (auto kp = first_step_on_level(levelp-1); 
                                        kp < first_step_on_level(levelp); kp++) {
                                    if (kp == idx) {
                                        pLits[1] = abc::Abc_Var2Lit(
                                                spec.selection_vars[ip][j][kp],1);
                                        solver_add_clause(this->solver,
                                                pLits, pLits+2);
                                        pLits[1] = abc::Abc_Var2Lit(
                                                spec.selection_vars[ip][k][kp], 1);
                                        solver_add_clause(this->solver,
                                                pLits, pLits+2);
                                    }
                                }
                            }
                        }
                    }
                }
            }
*/

            /// Extracts chain from encoded CNF solution.
            template<typename TT>
            void 
            extract_chain(synth_spec<TT>& spec, chain<FI>& chain)
            {
                fanin op_inputs[FI];

                chain.reset(spec.get_nr_in(), spec.get_nr_out(), spec.nr_steps);

                auto svar_offset = 0;
                for (int i = 0; i < spec.nr_steps; i++) {
                    kitty::static_truth_table<FI> op;
                    for (int j = 1; j <= nr_op_vars_per_step; j++) {
                        if (solver_var_value(*solver, get_op_var(spec, i, j))) {
                            kitty::set_bit(op, j); 
                        }
                    }

                    if (spec.verbosity) {
                        printf("  step x_%d performs operation\n  ", 
                                i+spec.get_nr_in()+1);
                        kitty::print_binary(op, std::cout);
                        printf("\n");
                    }

                    const auto nr_svars_for_i = nr_svar_map[i];
                    for (int j = 0; j < nr_svars_for_i; j++) {
                        const auto sel_var = get_sel_var(svar_offset + j);
                        if (solver_var_value(*solver, sel_var)) {
                            const auto& fanins = svar_map[svar_offset + j];
                            if (spec.verbosity) {
                                printf("  with operands ");
                                for (int k = 0; k < FI; k++) {
                                    printf("x_%d ", fanins[k] + 1);
                                }
                            }
                            chain.set_step(i, fanins, op);
                            break;
                        }
                    }

                    svar_offset += nr_svars_for_i;
                        
                    if (spec.verbosity) {
                        printf("\n");
                    }
                }

                auto triv_count = 0, nontriv_count = 0;
                for (int h = 0; h < spec.get_nr_out(); h++) {
                    if ((spec.triv_flag >> h) & 1) {
                        chain.set_output(h, (spec.triv_functions[triv_count++] << 1) +
                                ((spec.out_inv >> h) & 1));
                        continue;
                    }
                    for (int i = 0; i < spec.nr_steps; i++) {
                        if (solver_var_value(*solver, 
                                    get_out_var(spec, nontriv_count, i))) {
                            chain.set_output(h, ((i + spec.get_nr_in() + 1) << 1) +
                                    ((spec.out_inv >> h) & 1));
                            nontriv_count++;
                            break;
                        }
                    }
                }
            }

/*
            template<typename TT>
            void 
            create_colex_clauses(const synth_spec<TT>& spec)
            {
                int pLits[2];

                for (int i = 0; i < spec.nr_steps-1; i++) {
                    const auto level = get_level(spec, i + spec.get_nr_in());
                    const auto levelp = get_level(spec, i + 1 +
                            spec.get_nr_in());
                    for (auto k = first_step_on_level(level-1); 
                            k < first_step_on_level(level); k++) {
                        if (k < 2) continue;
                        for (auto kp = first_step_on_level(levelp-1); 
                                kp < first_step_on_level(levelp); kp++) {
                            if (kp != k) {
                                continue;
                            }
                            for (int j = 1; j < k; j++) {
                                for (int jp = 0; jp < j; jp++) {
                                    pLits[0] = abc::Abc_Var2Lit(spec.selection_vars[i][j][k], 1);
                                    pLits[1] = abc::Abc_Var2Lit(
                                            spec.selection_vars[i+1][jp][k], 1);
                                    solver_add_clause(
                                            this->solver, pLits, pLits+2);
                                }
                            }
                        }
                        const auto min_kp = first_step_on_level(levelp-1);
                        const auto max_kp = first_step_on_level(levelp);
                        for (int j = 0; j < k; j++) {
                            for (int kp = 1; kp < k; kp++) {
                                if (kp < min_kp || kp >= max_kp) {
                                    // Node ip would never select these j and kp
                                    // anyway.
                                    continue;
                                }
                                for (int jp = 0; jp < kp; jp++) {
                                    pLits[0] = abc::Abc_Var2Lit(
                                            spec.selection_vars[i][j][k], 1);
                                    pLits[1] = abc::Abc_Var2Lit(
                                            spec.selection_vars[i+1][jp][kp],1);
                                    solver_add_clause(
                                            this->solver, pLits, pLits+2);
                                }
                            }
                        }
                    }
                }
            }

            template<typename TT>
            void 
            create_colex_func_clauses(const synth_spec<TT>& spec)
            {
                for (int i = 0; i < spec.nr_steps-1; i++) {
                    for (int k = 1; k < spec.get_nr_in()+i; k++) {
                        for (int j = 0; j < k; j++) {
                            pLits[0] = abc::Abc_Var2Lit(spec.selection_vars[i][j][k], 1);
                            pLits[1] = abc::Abc_Var2Lit(spec.selection_vars[i+1][j][k], 1);

                            pLits[2] = 
                                abc::Abc_Var2Lit(get_op_var(spec, i, 1, 1), 1);
                            pLits[3] = 
                                abc::Abc_Var2Lit(get_op_var(spec, i+1, 1, 1), 0);
                            solver_add_clause(this->solver, pLits, pLits+4);

                            pLits[3] = 
                                abc::Abc_Var2Lit(get_op_var(spec, i, 1, 0), 1);
                            pLits[4] = 
                                abc::Abc_Var2Lit(get_op_var(spec, i+1, 1, 1), 0);
                            solver_add_clause(this->solver, pLits, pLits+5);
                            pLits[4] = 
                                abc::Abc_Var2Lit(get_op_var(spec, i+1, 1, 0), 0);
                            solver_add_clause(this->solver, pLits, pLits+5);

                            pLits[4] = 
                                abc::Abc_Var2Lit(get_op_var(spec, i, 0, 1), 1);
                            pLits[5] = 
                                abc::Abc_Var2Lit(get_op_var(spec, i+1, 1, 1), 0);
                            solver_add_clause(this->solver, pLits, pLits+6);
                            pLits[5] = 
                                abc::Abc_Var2Lit(get_op_var(spec, i+1, 1, 0), 0);
                            solver_add_clause(this->solver, pLits, pLits+6);
                            pLits[5] = 
                                abc::Abc_Var2Lit(get_op_var(spec, i+1, 0, 1), 0);
                            solver_add_clause(this->solver, pLits, pLits+6);
                        }
                    }
                }
            }

            template<typename TT>
            void 
            create_symvar_clauses(const synth_spec<TT>& spec)
            {
                for (int q = 1; q < spec.get_nr_in(); q++) {
                    for (int p = 0; p < q; p++) {
                        auto symm = true;
                        for (int i = 0; i < spec.get_nr_out(); i++) {
                            auto outfunc = spec.functions[i];
                            if (!(swap(*outfunc, p, q) == *outfunc)) {
                                symm = false;
                                break;
                            }
                        }
                        if (!symm) {
                            continue;
                        }
                        if (spec.verbosity) {
                            printf("  variables x_%d and x_%d are symmetric\n",
                                    p+1, q+1);
                        }
                        for (int i = 0; i < spec.nr_steps; i++) {
                            const auto level = get_level(spec,
                                    i+spec.get_nr_in());
                            if (level > 1) {
                                // Any node on level 2 or higher could never
                                // refer to variable q as its second input.
                                continue;
                            }
                            for (int j = 0; j < q; j++) {
                                if (j == p) continue;

                                auto slit = abc::Abc_Var2Lit(spec.selection_vars[i][j][q], 1);
                                abc::Vec_IntSetEntry(vLits, 0, slit);

                                int ctr = 1;
                                for (int ip = 0; ip < i; ip++) {
                                    const auto levelp = get_level(spec, ip+spec.get_nr_in());
                                    for (auto kp = first_step_on_level(levelp-1); 
                                            kp < first_step_on_level(levelp); kp++) {
                                        for (int jp = 0; jp < kp; jp++) {
                                            if (jp == p || kp == p) {
                                                slit = abc::Abc_Var2Lit(
                                                    spec.selection_vars[ip][jp][kp], 0);
                                                abc::Vec_IntSetEntry(vLits, ctr++, slit);
                                                solver_add_clause(
                                                    this->solver,
                                                    abc::Vec_IntArray(vLits),
                                                    abc::Vec_IntArray(vLits)
                                                    + ctr);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
*/

			/// Encodes specifciation for use in standard synthesis flow.
            template<typename TT>
            bool 
            encode(const synth_spec<TT>& spec, const fence& f)
            {
                assert(spec.nr_steps <= MAX_STEPS);

                bool success = true;

                update_level_map(spec, f);
                create_variables(spec);
                success = create_main_clauses(spec);
                if (!success) {
                    return false;
                }

                create_output_clauses(spec);
                create_op_clauses(spec);

                if (spec.add_nontriv_clauses) {
                    create_nontriv_clauses(spec);
                }

                if (spec.add_alonce_clauses) {
                    create_alonce_clauses(spec);
                }
                /*
                if (spec.add_noreapply_clauses) {
                    create_noreapply_clauses(spec);
                }
                if (spec.add_colex_clauses) {
                    create_colex_clauses(spec);
                }
                if (spec.add_colex_func_clauses) {
                    create_colex_func_clauses(spec);
                }
                if (spec.add_symvar_clauses) {
                    create_symvar_clauses(spec);
                }
*/

                return true;
            }

			/// Encodes specifciation for use in CEGAR based synthesis flow.
            template<typename TT>
            bool 
            cegar_encode(const synth_spec<TT>& spec, const fence& f)
            {
                assert(spec.nr_steps <= MAX_STEPS);

                update_level_map(spec, f);
                create_variables(spec);
                for (int i = 0; i < spec.nr_rand_tt_assigns; i++) {
                    if (!create_tt_clauses(spec, rand()%spec.get_tt_size())) {
                        return false;
                    }
                }
                
                create_output_clauses(spec);
                create_op_clauses(spec);

                if (spec.add_nontriv_clauses) {
                    create_nontriv_clauses(spec);
                }
                if (spec.add_alonce_clauses) {
                    create_alonce_clauses(spec);
                }
                /*
                if (spec.add_noreapply_clauses) {
                    create_noreapply_clauses(spec);
                }
                if (spec.add_colex_clauses) {
                    create_colex_clauses(spec);
                }
                if (spec.add_colex_func_clauses) {
                    create_colex_func_clauses(spec);
                }
                if (spec.add_symvar_clauses) {
                    create_symvar_clauses(spec);
                }
                */

                return true;
            }

            /// Assumes that a solution has been found by the current encoding.
            /// Blocks the current solution such that the solver is forced to
            /// find different ones (if they exist).
            template<typename TT>
            bool
            block_solution(const synth_spec<TT>& spec)
            {
                int ctr = 0;
                int svar_offset = 0;

                for (int i = 0; i < spec.nr_steps; i++) {
                    for (int j = 1; j <= nr_op_vars_per_step; j++) {
                        int invert = 0;
                        const auto op_var = get_op_var(spec, i, j);
                        if (solver_var_value(*solver, op_var)) {
                            invert = 1;
                        }
                        abc::Vec_IntSetEntry(vLits, ctr++,
                                abc::Abc_Var2Lit(get_op_var(spec, i, j),
                                    invert));
                    }

                    const auto nr_svars_for_i = nr_svar_map[i];
                    for (int j = 0; j < nr_svars_for_i; j++) {
                        const auto sel_var = get_sel_var(svar_offset + j);
                        if (solver_var_value(*solver, sel_var)) {
                            abc::Vec_IntSetEntry(vLits, ctr++,
                                    abc::Abc_Var2Lit(sel_var, 1));
                            break;
                        }
                    }

                    svar_offset += nr_svars_for_i;
                }
                
                return solver_add_clause(
                            *solver,
                            abc::Vec_IntArray(vLits), 
                            abc::Vec_IntArray(vLits) + ctr);
            }


            /// Similar to block_solution, but blocks all solutions with the
            /// same structure. This is more restrictive, since the other
            /// method allows for the same structure but different operators.
            template<typename TT>
            bool
            block_struct_solution(const synth_spec<TT>& spec)
            {
                int ctr = 0;
                int svar_offset = 0;

                for (int i = 0; i < spec.nr_steps; i++) {
                    const auto nr_svars_for_i = nr_svar_map[i];
                    for (int j = 0; j < nr_svars_for_i; j++) {
                        const auto sel_var = get_sel_var(svar_offset + j);
                        if (solver_var_value(*solver, sel_var)) {
                            abc::Vec_IntSetEntry(vLits, ctr++,
                                    abc::Abc_Var2Lit(sel_var, 1));
                            break;
                        }
                    }

                    svar_offset += nr_svars_for_i;
                }

                return solver_add_clause(
                            *solver,
                            abc::Vec_IntArray(vLits), 
                            abc::Vec_IntArray(vLits) + ctr);
            }

            
    };
}

