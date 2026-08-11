import React from "react";
import ReactDOM from "react-dom/client";
import App from "./App";
import "./theme.css";
import { applyTheme } from "./theme";

// Before first render: without [data-theme] no token block matches and
// every var() resolves empty.
applyTheme("dark");

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
