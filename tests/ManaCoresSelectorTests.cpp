// tests/ManaCoresSelectorTests.cpp
#include <gtest/gtest.h>
#include "mana_core/ManaCoresSelector.hpp"
#include <gdk/gdk.h>

TEST(ManaCoresSelector, Constructs) {
    realmheart::mana_core::ManaCoresSelector sel;
    EXPECT_FALSE(sel.is_visible());
}

TEST(ManaCoresSelector, DismissCallbackInvokedOnDismiss) {
    realmheart::mana_core::ManaCoresSelector sel;
    bool dismissed = false;
    sel.set_dismiss_callback([&dismissed]() {
        dismissed = true;
    });
    sel.dismiss();
    EXPECT_TRUE(dismissed);
    EXPECT_FALSE(sel.is_visible());
}

TEST(ManaCoresSelector, ApplyCallbackWiring) {
    realmheart::mana_core::ManaCoresSelector sel;
    std::string applied_path;
    sel.set_apply_callback([&applied_path](const std::string& path) {
        applied_path = path;
    });
    // Verify callback setter does not crash
    EXPECT_FALSE(sel.is_visible());
}

TEST(ManaCoresSelector, HandleKeyWhenHiddenReturnsFalse) {
    realmheart::mana_core::ManaCoresSelector sel;
    EXPECT_FALSE(sel.handle_key(GDK_KEY_Escape));
    EXPECT_FALSE(sel.handle_key(GDK_KEY_Return));
    EXPECT_FALSE(sel.handle_key(GDK_KEY_Left));
    EXPECT_FALSE(sel.handle_key(GDK_KEY_Right));
}