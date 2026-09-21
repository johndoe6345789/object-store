import next from "eslint-config-next";

export default [
  ...next,
  { ignores: [".next/**", "node_modules/**"] },
  {
    rules: {
      // React 19 compiler lints the existing hooks trip on (fetch-on-mount
      // effects, a ref carried inside the form-state object): reported, not
      // blocking, until those hooks are reworked.
      "react-hooks/set-state-in-effect": "warn",
      "react-hooks/refs": "warn",
      // `Image` here is an icon component, not <img>.
      "jsx-a11y/alt-text": "off",
    },
  },
];
