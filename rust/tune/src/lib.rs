//! Typed, accessible configuration and control menus for `USBRadioPlus`.

#![allow(
    clippy::literal_string_with_formatting_args,
    reason = "Clippy misclassifies the conditional coverage feature attribute"
)]
#![cfg_attr(coverage, feature(coverage_attribute))]

pub mod arguments;
pub mod catalog;
pub mod control;
pub mod menus;
pub mod policy;
pub mod repository;
pub mod ui;
