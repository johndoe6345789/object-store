/**
 * @file ui/index.ts
 * @brief The UI kit the app imports from.
 *
 * Stands in for the sibling-repo `@metabuilder/m3` package, which this repo
 * cannot depend on when built alone. Components are Material UI (the same
 * names and props the app already used); icons are `@mui/icons-material`.
 * Grid is MUI's legacy (xs/sm/md props) Grid; File is the generic file icon.
 */

export {
  Alert,
  AppBar,
  Box,
  Breadcrumbs,
  Button,
  Card,
  CardActions,
  CardContent,
  Chip,
  CircularProgress,
  Container,
  Dialog,
  DialogActions,
  DialogContent,
  DialogTitle,
  Divider,
  Drawer,
  GridLegacy as Grid,
  IconButton,
  Link,
  List,
  ListItemButton,
  ListItemIcon,
  ListItemText,
  Paper,
  Stack,
  Table,
  TableBody,
  TableCell,
  TableContainer,
  TableHead,
  TableRow,
  TextField,
  Toolbar,
  Typography,
} from '@mui/material';

export { default as Add } from '@mui/icons-material/Add';
export { default as Archive } from '@mui/icons-material/Archive';
export { default as Close } from '@mui/icons-material/Close';
export { default as Cloud } from '@mui/icons-material/Cloud';
export { default as CloudQueue } from '@mui/icons-material/CloudQueue';
export { default as Code } from '@mui/icons-material/Code';
export { default as Dashboard } from '@mui/icons-material/Dashboard';
export { default as Delete } from '@mui/icons-material/Delete';
export { default as Description } from '@mui/icons-material/Description';
export { default as Download } from '@mui/icons-material/Download';
export { default as Email } from '@mui/icons-material/Email';
export { default as Folder } from '@mui/icons-material/Folder';
export { default as Home } from '@mui/icons-material/Home';
export { default as Image } from '@mui/icons-material/Image';
export { default as Logout } from '@mui/icons-material/Logout';
export { default as OpenInNew } from '@mui/icons-material/OpenInNew';
export { default as Refresh } from '@mui/icons-material/Refresh';
export { default as Storage } from '@mui/icons-material/Storage';
export { default as Upload } from '@mui/icons-material/Upload';
export { default as ViewList } from '@mui/icons-material/ViewList';
export { default as File } from '@mui/icons-material/InsertDriveFile';
export { default as AppHeaderActions } from './AppHeaderActions';
