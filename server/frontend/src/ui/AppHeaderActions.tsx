'use client';

import { Button } from '@mui/material';
import Logout from '@mui/icons-material/Logout';

/** @brief Props for the header's sign-out control. */
interface AppHeaderActionsProps {
  /** Path of the current tool (kept for parity with the shared header). */
  activePath?: string;
  onLogout: () => void;
}

/**
 * @brief Minimal header actions: just sign out. The shared org header this
 * replaces also carried a burger/bell/theme switch that are not needed here.
 */
export default function AppHeaderActions({
  onLogout,
}: AppHeaderActionsProps) {
  return (
    <Button
      color="inherit"
      size="small"
      startIcon={<Logout />}
      onClick={onLogout}
      data-testid="logout-button"
    >
      Sign out
    </Button>
  );
}
