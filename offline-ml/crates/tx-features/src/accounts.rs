//! Transaction-level account-role rollups that do not need another byte scan.

use crate::view::AccountRoleV1;

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct UsageRollup {
    pub max_usage: u16,
    pub max_writable_usage: u16,
    pub accounts_used_by_multiple: u16,
    pub writable_accounts_used_by_multiple: u16,
    pub unique_instruction_accounts: u16,
    pub unique_programs: u16,
    pub repeated_program_invocations: u16,
}

pub(crate) fn rollup(accounts: &[AccountRoleV1]) -> UsageRollup {
    let mut out = UsageRollup::default();
    for a in accounts {
        if a.usage_count > out.max_usage {
            out.max_usage = a.usage_count;
        }
        if a.writable_usage_count > out.max_writable_usage {
            out.max_writable_usage = a.writable_usage_count;
        }
        if a.usage_count > 1 {
            out.accounts_used_by_multiple = out.accounts_used_by_multiple.saturating_add(1);
        }
        if a.writable_usage_count > 1 {
            out.writable_accounts_used_by_multiple =
                out.writable_accounts_used_by_multiple.saturating_add(1);
        }
        if a.used_as_instruction_account == 1 {
            out.unique_instruction_accounts = out.unique_instruction_accounts.saturating_add(1);
        }
        if a.program_usage_count > 0 {
            out.unique_programs = out.unique_programs.saturating_add(1);
        }
        if a.program_usage_count > 1 {
            out.repeated_program_invocations = out
                .repeated_program_invocations
                .saturating_add(a.program_usage_count - 1);
        }
    }
    out
}
