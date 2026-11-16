#include "alert_policy.h"
#include "brain/adapt.h"
#include "brain/fusion.h"
#include "brain/model_a.h"
#include "brain/mq.h"
#include "emergency_policy.h"
#include "vision_mapping.h"
#include <assert.h>
#include <iostream>
#include <math.h>
using namespace alert_policy;
static Intent step(Policy &p, Input i) {
    p.track(i);
    auto n = p.next(i);
    if (n.kind != NONE)
        p.accepted(n, i);
    return n;
}
int main() {
    {
        emergency_policy::Policy p(100);
        p.track(true, false, false, 1000);
        assert(p.event() == 101);
        assert(p.next(false, true, false, 60000, 70000) == emergency_policy::NONE);
        assert(p.next(true, false, false, 60000, 70000) == emergency_policy::NONE);
        assert(p.next(true, true, false, 60000, 60999) == emergency_policy::NONE);
        assert(p.next(true, true, false, 60000, 61000) == emergency_policy::UNATTENDED);
        assert(!p.acknowledge(100));
        assert(p.acknowledge(101));
        assert(p.next(true, true, false, 60000, 62000) == emergency_policy::NONE);
        assert(p.next(true, true, true, 60000, 62000) == emergency_policy::HIGH_FIRE);
        p.accepted();
        assert(p.next(true, true, true, 60000, 63000) == emergency_policy::NONE);
        p.track(false, false, true, 64000);
        assert(!p.event());
        p.track(true, false, false, 65000);
        assert(p.event() == 102 && !p.acknowledged());
    }
    {
        emergency_policy::Policy p;
        uint32_t t = 0xfffffff0u;
        p.track(true, false, false, t);
        assert(p.next(true, true, false, 60000, t + 60000) == emergency_policy::UNATTENDED);
    }

    {
        Policy p;
        assert(step(p, {1, false, false, 100}).kind == NONE);
        assert(step(p, {9, false, false, 1000}).kind == FIRE);
        assert(step(p, {9, false, false, 60999}).kind == NONE);
        assert(step(p, {9, false, true, 61000}).kind == NONE);
        assert(step(p, {9, false, false, 62000}).kind == ESCALATE);
        assert(step(p, {9, false, false, 63000}).kind == NONE);
        assert(step(p, {9, true, false, 64000}).kind ==
               FIRE); // confirmation upgrades even without state change
        assert(step(p, {11, false, false, 65000}).kind == NONE); // recovery is not all clear
        assert(step(p, {1, false, false, 70000}).kind == ALL_CLEAR);
        assert(step(p, {1, false, false, 71000}).kind == NONE);
        assert(step(p, {6, false, false, 72000}).kind == GAS);
    }
    {
        Policy p;
        assert(step(p, {8, false, false, 100}).kind == POSSIBLE);
        for (unsigned n = 1; n < Policy::MAX_ROUNDS; n++)
            assert(step(p, {8, false, false, 100 + 180000 * n}).kind == UPDATE);
        assert(step(p, {8, false, false, 100 + 180000 * Policy::MAX_ROUNDS}).kind == NONE);
        assert(step(p, {1, false, false, 2000000}).kind == ALL_CLEAR);
    }
    {
        Policy p;
        uint32_t t = 0xffff0000U;
        assert(step(p, {9, false, false, t}).kind == FIRE);
        assert(step(p, {9, false, false, t + 60000}).kind == ESCALATE);
    }
    {
        Policy p;
        Input i{9, false, false, 100};
        p.track(i);
        auto n = p.next(i);
        assert(n.kind == FIRE);
        assert(p.next(i).kind == FIRE); // unaccepted/full queue must be retried
        p.accepted(n, i);
        assert(p.next(i).kind == NONE);
    }
    {
        VisionOut c;
        c.conf = 0.7f;
        c.personConf = 0.3f;
        Verdict v;
        v.det[v.count++] = {0, 0.9f, 130, 190, 160, 220}; // small low burner
        v.det[v.count++] = {0, 0.8f, 20, 20, 220, 200};   // separate open flame
        v.det[v.count++] = {1, 0.85f, 0, 0, 160, 120};
        auto o = map_yolo(c, v, 320, 240);
        assert(o.burnerFlame == 0.9f && o.openFlame == 0.8f && o.smokeHeavy == 0.85f && o.conf == 0.7f);
        v.count = 1;
        v.det[0] = {0, 0.8f, 0, 0, 20, 20};
        assert(map_yolo(c, v, 320, 240).openFlame == 0.8f); // small but high = fire
        v.det[0] = {0, 0.8f, 0, 150, 320, 240};
        assert(map_yolo(c, v, 320, 240).openFlame == 0.8f); // low but large = fire
        c.conf = 0;
        assert(map_yolo(c, v, 320, 240).conf == 0);
    }
    {
        Adapt a;
        for (auto &v : a.alpha)
            v = ADAPT_ALPHA_MAX;
        for (int c = 0; c < AD_N; c++) {
            assert(a.scale(AdaptChan(c), ADAPT_HARD_KEEP) == 1.0f);
            assert(a.scale(AdaptChan(c), 1.0f) == 1.0f);
            assert(a.scale(AdaptChan(c), 0) >= 1 - ADAPT_ALPHA_MAX - 1e-6);
        }
    }
    {
        MQChannel m;
        m.begin(1, 0);
        test_mv = 1000;
        assert(m.sample());
        assert(fabs(m.dev) < 1e-6);
        float base = m.baseline;
        test_mv = 1400;
        assert(m.sample() && m.dev > 0);
        m.updateBaseline(60, false);
        assert(m.baseline == base);
        test_mv = 0;
        assert(!m.sample());
    }
    {
        FeatureRing r;
        float d[3] = {0, 0, 0}, sc[3] = {0.05f, 0.05f, 0.05f}, f[N_FEAT];
        for (int n = 0; n <= 120; n++)
            r.push(d, 25 + n / 40.0f, 10 + n / 60.0f);
        r.compute(f, sc, 0, 1);
        assert(fabs(f[20] - 3) < 1e-5 && fabs(f[21] - 2) < 1e-5);
    }
    {
        Fusion f;
        f.begin(1);
        for (int c = 0; c < N_CHAN; c++) {
            f.raw[c] = 1;
            f.w[c] = 1;
            f.ctx[c] = 1;
        }
        for (uint32_t t = 500; t < 120000; t += 500) {
            f.condition(.5);
            f.aggregate(.5);
            f.auxScores(true);
            f.updateState(t, false, false);
        }
        assert(f.frs >= 96 && f.state == ST_CRITICAL);
        f.updateState(120000, false, true);
        assert(f.state == ST_CRITICAL); // channel fault cannot demote hazard
        Fusion idle;
        idle.begin(1);
        idle.updateState(1000, false, true);
        assert(idle.state == ST_SENSOR_ERROR);
    }
    std::cout << "Core policy, mapping, MQ, features, adaptation and fusion tests passed\n";
}
