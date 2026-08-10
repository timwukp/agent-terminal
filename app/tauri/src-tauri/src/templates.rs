//! New-session templates: compiled-in defaults, overridable by the user
//! at ~/.agent-terminal/gui-templates.json (design: ux-spec.md). The
//! override REPLACES the defaults when present and valid — merging two
//! lists gives no way to remove a default — and a malformed file is
//! reported, not silently ignored.

use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Clone, PartialEq, Debug)]
pub struct Template {
    pub label: String,
    pub name_prefix: String,
    /// Empty argv → the daemon runs $SHELL (its own default).
    pub argv: Vec<String>,
}

#[derive(Deserialize)]
struct TemplateFile {
    templates: Vec<Template>,
}

fn defaults() -> Vec<Template> {
    vec![
        Template {
            label: "New Claude session".into(),
            name_prefix: "claude".into(),
            argv: vec!["claude".into()],
        },
        Template {
            label: "New shell".into(),
            name_prefix: "shell".into(),
            argv: vec![],
        },
    ]
}

/// Pure core, unit-tested: defaults when there is no override; the
/// override's templates when it parses; an error when it exists but is
/// malformed (the user edited it — swallowing that hides their bug).
pub fn resolve(override_json: Option<&str>) -> Result<Vec<Template>, String> {
    match override_json {
        None => Ok(defaults()),
        Some(s) => serde_json::from_str::<TemplateFile>(s)
            .map(|f| f.templates)
            .map_err(|e| format!("gui-templates.json is malformed: {e}")),
    }
}

#[tauri::command]
pub fn list_templates() -> Result<Vec<Template>, String> {
    let path = std::path::PathBuf::from(std::env::var("HOME").unwrap_or_default())
        .join(".agent-terminal")
        .join("gui-templates.json");
    let contents = std::fs::read_to_string(&path).ok();
    resolve(contents.as_deref())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn no_override_gives_defaults() {
        let t = resolve(None).unwrap();
        assert_eq!(t.len(), 2);
        assert_eq!(t[0].argv, vec!["claude"]);
        assert!(t[1].argv.is_empty());
    }

    #[test]
    fn override_replaces_not_merges() {
        let json =
            r#"{"templates":[{"label":"SSH prod","name_prefix":"prod","argv":["ssh","prod"]}]}"#;
        let t = resolve(Some(json)).unwrap();
        assert_eq!(t.len(), 1);
        assert_eq!(t[0].argv, vec!["ssh", "prod"]);
    }

    #[test]
    fn malformed_override_is_an_error_not_defaults() {
        assert!(resolve(Some("{not json")).is_err());
        assert!(resolve(Some(r#"{"templates": "nope"}"#)).is_err());
    }
}
