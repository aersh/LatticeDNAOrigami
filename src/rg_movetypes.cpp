// rg_movetypes.cpp

#include <rg_movetypes.h>

namespace movetypes {

    using utility::all_pairs;
    using utility::pair_to_index;
    using utility::Occupancy;

    CTScaffoldRG::CTScaffoldRG(
            OrigamiSystem& origami_system,
            RandomGens& random_gens,
            IdealRandomWalks& ideal_random_walks,
            vector<OrigamiOutputFile*> config_files,
            string label,
            SystemOrderParams& ops,
            SystemBiases& biases,
            InputParameters& params,
            int num_excluded_staples,
            int max_recoils):

        MCMovetype(origami_system, random_gens, ideal_random_walks,
                config_files, label, ops, biases, params),
        CTRegrowthMCMovetype(origami_system, random_gens, ideal_random_walks,
                config_files, label, ops, biases, params, num_excluded_staples),
        m_all_configs {all_pairs<VectorThree>(utility::vectors)},
        m_config_to_i {pair_to_index<VectorThree>(m_all_configs)},
        m_all_cis (m_all_configs.size()),
        m_max_recoils {max_recoils} {

        std::iota(m_all_cis.begin(), m_all_cis.end(), 0);
    }

    void CTScaffoldRG::write_log_summary(ostream*) {
    }

    void CTScaffoldRG::reset_internal() {
        MCMovetype::reset_internal();
        m_sel_scaf_doms.clear();
        m_regrow_ds.clear();
        m_c_attempts_q.clear();
        m_c_attempts_wq.clear();
        m_avail_cis_q.clear();
        m_avail_cis_wq.clear();
        m_c_opens.clear();
        m_old_pos.clear();
        m_old_ore.clear();
        m_delta_e = 0;
        m_delta_e_new = 0;
        m_weight = 1;
        m_weight_new = 1;
    }

    void CTScaffoldRG::add_external_bias() {
        m_ops.update_move_params();
        m_delta_e += m_biases.calc_move();
    }

    bool CTScaffoldRG::internal_attempt_move() {
        bool accepted {false};

        // Select scaffold indices and excluded staples
        m_sel_scaf_doms = select_indices(m_scaffold);
        sel_excluded_staples();

        // Setup up constraint points
        // Get domains to be regrown
        m_regrow_ds = m_constraintpoints.domains_to_be_regrown();
        //m_tracker.num_staples = staples.size();

        // Recoil regrow
        unassign_and_save_domains();
        recoil_regrow();

        // Reject if constraints disobeyed
        if (m_rejected) {
            accepted = false;
            return accepted;
        }

        // Reject if excluded staples not bound to system
        m_rejected = excluded_staples_bound();
        if (m_rejected) {
            accepted = false;
            return accepted;
        }
        add_external_bias();

        // Calculate new weights
        setup_for_calc_new_weights();
        unassign_and_save_domains();
        calc_weights();

        // Calculate old weights
        unassign_domains();
        calc_old_c_opens();
        add_external_bias();
        setup_for_calc_old_weights();
        unassign_and_save_domains();
        calc_weights();

        // Test acceptance
        accepted = test_rg_acceptance();

        return accepted;
    }

    void CTScaffoldRG::add_tracker(bool) {
    }

    void CTScaffoldRG::unassign_and_save_domains() {
        for (auto domain: m_regrow_ds) {
            pair<int, int> key {domain->m_c, domain->m_d};
            m_prev_pos[key] = domain->m_pos;
            m_prev_ore[key] = domain->m_ore;
            m_modified_domains.push_back(key);
            m_origami_system.unassign_domain(*domain);
        }
        write_config();
        update_external_bias();
    }

    void CTScaffoldRG::unassign_domains() {
        for (auto domain: m_regrow_ds) {
            m_origami_system.unassign_domain(*domain);
        }
        write_config();
        update_external_bias();
    }

    void CTScaffoldRG::update_external_bias() {
        m_ops.update_move_params();
        m_biases.calc_move();
    }

    void CTScaffoldRG::setup_constraints() {
        m_constraintpoints.calculate_constraintpoints(m_sel_scaf_doms,
                m_excluded_staples);

        // If scaffold is cyclic and the whole range is selected, the endpoints
        // on the first domain are needed, otherwise remove
        if (not m_origami_system.m_cyclic and m_sel_scaf_doms.size() !=
                m_origami_system.get_chain(0).size()) {

            m_constraintpoints.remove_active_endpoint(m_sel_scaf_doms[0]);
        }
    }

    void CTScaffoldRG::setup_for_calc_new_weights() {
        m_c_attempts_wq = m_c_attempts_q;
        m_avail_cis_wq = m_avail_cis_q;
        m_old_pos = m_prev_pos;
        m_old_ore = m_prev_ore;
        m_modified_domains.clear();
    }

    void CTScaffoldRG::setup_for_calc_old_weights() {
        m_delta_e_new = m_delta_e;
        m_delta_e = 0;
        m_weight_new = m_weight;
        m_weight = 1;
        m_c_attempts_wq = m_c_attempts_q;
        m_avail_cis_wq = m_avail_cis_q;
        m_modified_domains.clear();
    }

    /*
     * For each domain, I will need to pick a trial config that hasn't
     * been tried before. Since the number of possible is enumerable, I
     * keep a constant list of all possible and mutable lists of those
     * that haven't been tried yet for each domain to be grown.
     *
     * To grow each domain, a reference config and the direction of growth
     * along the chain is needed. If the domain is the first to be grown of
     * a new chain, then reference config is the position of the domain it
     * is being grown out from, which is always the previous domain in the
     * regrowth stack. Otherwise, the reference config is the adjacent
     * domain in the reverse direction of growth.
     */
    void CTScaffoldRG::recoil_regrow() {

        m_di = 0;
        m_d = m_regrow_ds[m_di];
        m_dir = m_regrow_ds[1]->m_d - m_regrow_ds[0]->m_d;
        m_c_attempts_q.resize(m_regrow_ds.size());
        m_avail_cis.resize(m_regrow_ds.size());
        prepare_for_growth();

        // Stop growing when all domains done or max num recoils reached
        int recoils {0}; // Current number of recoil steps that have been taken
        while (true) {
            configT c; // Trial config for current domain
            double p_c_open;
            bool c_open {false}; // Is the config open
            while (not c_open and m_c_attempts != m_d_max_c_attempts) {
                m_c_attempts++;
                c = select_trial_config();
                m_c_opens[m_di] = c_open;
                p_c_open = calc_p_config_open(c);
                c_open = test_config_open(p_c_open); 
            }
            if (c_open) {

                // One less recoil unless aleady none
                if (recoils != 0) {
                    recoils--;
                }
                set_config(m_d, c);
                m_c_attempts_q[m_di] = m_c_attempts;
                m_avail_cis_q[m_di] = m_avail_cis;
                m_c_opens[m_di] = p_c_open;

                if (m_di == m_regrow_ds.size()) {
                    break;
                }
                else {
                    prepare_for_growth();
                }
            }
            else {
                m_origami_system.unassign_domain(*m_d);
                m_assigned_domains.pop_back();
                if (recoils == m_max_recoils or m_di == 1) {
                    break;
                }
                else {
                    recoils++;
                    prepare_for_regrowth();
                }
            }
        }
    }

    void CTScaffoldRG::set_config(Domain* d, configT c) {
        m_delta_e += m_origami_system.set_checked_domain_config(*d, c.first,
                c.second);
        pair<int, int> key {d->m_c, d->m_d};
        m_assigned_domains.push_back(key);
        write_config();
    }

    void CTScaffoldRG::prepare_for_growth() {
        m_prev_gp = m_constraintpoints.is_growthpoint(m_d);
        m_di++;
        m_d = m_regrow_ds[m_di];
        m_c_attempts = 0;
        if (m_prev_gp) {
            m_d_max_c_attempts = 1;
            m_avail_cis = {};
            m_dir = m_regrow_ds[m_di + 1]->m_d - m_d->m_d;
            m_ref_d = m_regrow_ds[m_di - 1];
        }
        else {
            m_d_max_c_attempts = m_max_c_attempts;
            m_avail_cis = m_all_cis;
            m_ref_d = (*m_d + m_dir);
        }
    }

    void CTScaffoldRG::prepare_for_regrowth() {
        m_di--;
        m_prev_gp = m_constraintpoints.is_growthpoint(
                m_regrow_ds[m_di - 1]);
        if (m_prev_gp) {
            m_d_max_c_attempts = 1;
            m_c_attempts = 1;
            m_avail_cis = {};
            m_dir = m_regrow_ds[m_di + 1]->m_d - m_d->m_d;
            m_ref_d = m_regrow_ds[m_di - 1];
        }
        else {
            m_d_max_c_attempts = m_max_c_attempts;
            m_c_attempts = m_c_attempts_q[m_di];
            m_avail_cis = m_avail_cis_q[m_di];
            m_ref_d = (*m_d + m_dir);
        }
    }

    configT CTScaffoldRG::select_trial_config() {
        configT c;
        int ci;
        if (m_prev_gp) {
            c = {m_ref_d->m_pos, -m_ref_d->m_ore};
        }
        else {
            ci = m_random_gens.uniform_int(0, m_avail_cis.size() - 1);
            std::list<int>::iterator it {std::next(m_avail_cis.begin(), ci)};
            int i {*it}; // TODO check this does what you think
            m_avail_cis.erase(it);
            configT c {m_all_configs[i]};
            c.first = c.first + m_ref_d->m_pos;
        }

        return c;
    }

    double CTScaffoldRG::calc_p_config_open(configT c) {
        double delta_e {m_origami_system.check_domain_constraints(*m_d,
                c.first, c.second)};
        if (m_origami_system.m_constraints_violated) {
            return 0;
        }
        if (not m_constraintpoints.walks_remain(m_d, c.first, m_dir)) {
            return 0;
        }

        // Consider putting this check into the constraintpoints class
        // It's repated in the CTCB code
        Occupancy pos_occ {m_origami_system.position_occupancy(c.first)};
        if (pos_occ == Occupancy::unbound) {
            Domain* occ_domain {m_origami_system.unbound_domain_at(c.first)};
            bool binding_same_chain {occ_domain->m_c == m_d->m_c};
            bool endpoint {m_constraintpoints.endpoint_reached(m_d, c.first)};
            bool excluded_staple {find(m_excluded_staples.begin(),
                    m_excluded_staples.end(), occ_domain->m_c) !=
                m_excluded_staples.end()};
            if (not (binding_same_chain or endpoint or excluded_staple)) {
                return 0;
            }
        }

        return std::exp(-delta_e);
    }

    bool CTScaffoldRG::test_config_open(double p_c_open) {
        p_c_open = std::fmin(1, p_c_open);
        bool c_open;
        if (p_c_open == 1) {
            c_open = true;
        }
        else {
            if (p_c_open > m_random_gens.uniform_real()) {
                c_open = true;
            }
            else {
                c_open = false;
            }
        }

        return c_open;
    }

    void CTScaffoldRG::calc_weights() {

        m_di = 0;
        while (m_di != m_regrow_ds.size()) {
            m_prev_gp = m_constraintpoints.is_growthpoint(m_d);
            m_di++;
            m_d = m_regrow_ds[m_di];

            // Already found the only avail config
            if (m_prev_gp) {
                m_dir = m_regrow_ds[m_di + 1]->m_d - m_d->m_d;
            }

            // Calculate the number of available configurations.
            else {
                m_ref_d = (*m_d + m_dir);
                int c_attempts {m_c_attempts_wq[m_di]};
                m_avail_cis = m_avail_cis_wq[m_di];
                int avail_cs {1}; // Available configurations, already found 1
                while (c_attempts != m_max_c_attempts) {
                    configT c {select_trial_config()};
                    double p_c_open {calc_p_config_open(c)};
                    if (test_config_open(p_c_open)) {
                        m_origami_system.set_checked_domain_config(*m_d,
                                c.first, c.second);
                        write_config();
                        avail_cs += test_config_avail();
                        m_origami_system.unassign_domain(*m_d);
                    }
                }
                m_weight *= avail_cs / m_c_opens[m_di];
            }

            // Set to actual config
            pair<int, int> key {m_d->m_c, m_d->m_d};
            VectorThree p {m_prev_pos[key]};
            VectorThree o {m_prev_pos[key]};
            m_origami_system.set_checked_domain_config(*m_d, p, o);
            write_config();
        }
    }

    /*
     * Grow domains out until the maximum number of recoils is reached, or
     * until it is determined that this cannot be done, using recoils.
     */
    bool CTScaffoldRG::test_config_avail() {
        prepare_for_growth();

        // Stop growing when all feeler grown or none possible
        bool c_avail;
        int feels {0}; // Current number of feeler domains grown
        while (true) {
            configT c; // Trial config for current domain
            bool c_open {false};
            bool p_c_open;
            while (not c_open and m_c_attempts != m_d_max_c_attempts) {
                m_c_attempts++;
                c = select_trial_config();
                p_c_open = calc_p_config_open(c); 
                c_open = test_config_open(p_c_open); 
            }
            if (c_open) {
                feels++;
                if (feels == m_max_recoils or m_di == m_regrow_ds.size()) {
                    feels--;
                    c_avail = true;
                    break;
                }
                else {
                    m_origami_system.set_checked_domain_config(*m_d, c.first,
                            c.second);
                     write_config();
                    m_c_attempts_q[m_di] = m_c_attempts;
                    m_avail_cis_q[m_di] = m_avail_cis;
                    prepare_for_growth();
                }
            }
            else {
                m_origami_system.unassign_domain(*m_d);
                if (feels == 0) {
                    c_avail = false;
                    break;
                }
                else {
                    feels--;
                    prepare_for_regrowth();
                }
            }
        }

        // Unassign feeler domains
        while (feels !=0) {
            m_di--;
            m_d = m_regrow_ds[m_di];
            m_origami_system.unassign_domain(*m_d);
        }

        return c_avail;
    }

    void CTScaffoldRG::calc_old_c_opens() {
        m_di = 0;
        while (m_di != m_regrow_ds.size()) {
            m_di++;
            m_d = m_regrow_ds[m_di];
            m_prev_gp = m_constraintpoints.is_growthpoint(
                m_regrow_ds[m_di - 1]);
            if (m_prev_gp) {
                m_ref_d = m_regrow_ds[m_di - 1];
            }
            else {
                m_ref_d = (*m_d + m_dir);
            }
            pair<int, int> key {m_d->m_c, m_d->m_d};
            VectorThree p {m_old_pos[key]};
            VectorThree o {m_old_ore[key]};
            p = p - m_ref_d->m_pos;
            configT c {p, o};
            m_c_opens[m_di]= calc_p_config_open(c);
            set_config(m_d, {p, o});
            m_c_attempts_q[m_di] = 1;

            // Remove this config from available
            int ci {m_config_to_i.at(c)};
            m_avail_cis_q[m_di].remove(ci);
        }
    }

    bool CTScaffoldRG::test_rg_acceptance() {
        double delta_e {m_delta_e_new = m_delta_e};
        long double ratio {m_weight_new / m_weight * std::exp(-delta_e)};
        bool accepted {false};
        if (test_acceptance(ratio)) {
            reset_origami();
            accepted = true;
        }

        // TODO change to match order of weight calculation
        else {
            m_modified_domains.clear();
            m_assigned_domains.clear();
            accepted = false;
        }

        return accepted;
    }
}