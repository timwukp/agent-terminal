import React from "react";
import ReactDOM from "react-dom/client";
import App from "./App";
import "./theme.css";
import { initTheme } from "./theme";

// Before first render: without [data-theme] no token block matches and
// every var() resolves empty. initTheme() also subscribes to the OS
// appearance, which is the default preference — the returned disposer is
// dropped on purpose, because this subscription lives as long as the
// window does.
initTheme();

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
