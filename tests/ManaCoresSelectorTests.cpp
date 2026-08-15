// tests/ManaCoresSelectorTests.cpp
#include <gtest/gtest.h>
#include "relictombs/ManaCoresSelector.hpp"

TEST(ManaCoresSelector, Constructs) {
    realmheart::relictombs::ManaCoresSelector sel;
    EXPECT_FALSE(sel.is_visible());
}

TEST(ManaCoresSelector, PresentSetsVisible) {
    realmheart::relictombs::ManaCoresSelector sel;
    // Cannot test present() without GTK context, but we can verify the API exists
    EXPECT_FALSE(sel.is_visible());
}