#pragma once

#include <gtest/gtest.h>

#include <string>

// Expects a core::Result to hold a value, printing the error message otherwise.
#define EXPECT_OK(expr)                                                                            \
    do {                                                                                           \
        const auto& studyappResult_ = (expr);                                                      \
        EXPECT_TRUE(studyappResult_.has_value())                                                   \
            << (studyappResult_.has_value() ? std::string() : studyappResult_.error().message);    \
    } while (false)

#define ASSERT_OK(expr)                                                                            \
    do {                                                                                           \
        const auto& studyappResult_ = (expr);                                                      \
        ASSERT_TRUE(studyappResult_.has_value())                                                   \
            << (studyappResult_.has_value() ? std::string() : studyappResult_.error().message);    \
    } while (false)
