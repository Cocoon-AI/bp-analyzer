package config

import (
	"fmt"
	"os"
	"path/filepath"
	"time"

	"gopkg.in/yaml.v3"
)

// Config holds the digbp configuration.
type Config struct {
	EditorCmd    string        `yaml:"editor_cmd"`
	UProject     string        `yaml:"uproject"`
	PipeName     string        `yaml:"pipe_name"`
	StartTimeout time.Duration `yaml:"start_timeout"`
}

// Defaults returns a Config with default values.
func Defaults() *Config {
	return &Config{
		PipeName:     "blueprintexport",
		StartTimeout: 120 * time.Second,
	}
}

// Load reads the config from ~/.digbp.yaml and merges with defaults.
func Load() *Config {
	cfg := Defaults()

	// Try config file
	configPath := configFilePath()
	if configPath != "" {
		data, err := os.ReadFile(configPath)
		if err == nil {
			_ = yaml.Unmarshal(data, cfg)
		}
	}

	// Apply env overrides
	if v := os.Getenv("DIGBP_EDITOR_CMD"); v != "" {
		cfg.EditorCmd = v
	}
	if v := os.Getenv("DIGBP_UPROJECT"); v != "" {
		cfg.UProject = v
	}
	if v := os.Getenv("DIGBP_PIPE_NAME"); v != "" {
		cfg.PipeName = v
	}

	// Ensure defaults for empty fields
	if cfg.PipeName == "" {
		cfg.PipeName = "blueprintexport"
	}
	if cfg.StartTimeout == 0 {
		cfg.StartTimeout = 120 * time.Second
	}

	return cfg
}

// Validate checks that required fields are set.
func (c *Config) Validate() error {
	if c.EditorCmd == "" {
		return fmt.Errorf("editor_cmd not set (use --editor-cmd flag, DIGBP_EDITOR_CMD env, or set in ~/.digbp.yaml)")
	}
	if c.UProject == "" {
		return fmt.Errorf("uproject not set (use --uproject flag, DIGBP_UPROJECT env, or set in ~/.digbp.yaml)")
	}
	return nil
}

func configFilePath() string {
	// Check DIGBP_CONFIG env first
	if v := os.Getenv("DIGBP_CONFIG"); v != "" {
		return v
	}

	home, err := os.UserHomeDir()
	if err != nil {
		return ""
	}
	return filepath.Join(home, ".digbp.yaml")
}
