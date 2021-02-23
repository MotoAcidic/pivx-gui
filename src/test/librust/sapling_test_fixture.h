// Copyright (c) 2020 The YieldStakingWallet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef YieldSakingWallet_SAPLING_TEST_FIXTURE_H
#define YieldSakingWallet_SAPLING_TEST_FIXTURE_H

#include "test/test_yieldstakingwallet.h"

/**
 * Testing setup that configures a complete environment for Sapling testing.
 */
struct SaplingTestingSetup : public TestingSetup {
    SaplingTestingSetup();
    ~SaplingTestingSetup();
};


#endif //YieldSakingWallet_SAPLING_TEST_FIXTURE_H
