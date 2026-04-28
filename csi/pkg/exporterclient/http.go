package exporterclient

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
)

type HTTPClient struct {
	baseURL string
	client  *http.Client
}

func NewHTTP(baseURL string) *HTTPClient {
	return &HTTPClient{
		baseURL: strings.TrimRight(baseURL, "/"),
		client:  &http.Client{},
	}
}

func (c *HTTPClient) CreateExport(ctx context.Context, req CreateExportRequest) (Export, error) {
	if err := req.Validate(); err != nil {
		return Export{}, err
	}
	var export Export
	if err := c.doJSON(ctx, http.MethodPost, "/v1/exports", req, http.StatusCreated, &export); err != nil {
		return Export{}, err
	}
	if err := export.Validate(); err != nil {
		return Export{}, err
	}
	return export, nil
}

func (c *HTTPClient) GetExport(ctx context.Context, exportID string) (Export, error) {
	if strings.TrimSpace(exportID) == "" {
		return Export{}, fmt.Errorf("export id is required")
	}
	var export Export
	if err := c.doJSON(ctx, http.MethodGet, "/v1/exports/"+exportID, nil, http.StatusOK, &export); err != nil {
		return Export{}, err
	}
	if err := export.Validate(); err != nil {
		return Export{}, err
	}
	return export, nil
}

func (c *HTTPClient) DeleteExport(ctx context.Context, exportID string) error {
	if strings.TrimSpace(exportID) == "" {
		return fmt.Errorf("export id is required")
	}
	return c.doJSON(ctx, http.MethodDelete, "/v1/exports/"+exportID, nil, http.StatusNoContent, nil)
}

func (c *HTTPClient) AllowHost(ctx context.Context, exportID, hostNQN string) error {
	if strings.TrimSpace(exportID) == "" {
		return fmt.Errorf("export id is required")
	}
	if strings.TrimSpace(hostNQN) == "" {
		return fmt.Errorf("host nqn is required")
	}
	return c.doJSON(ctx, http.MethodPost, "/v1/exports/"+exportID+"/allow-host", map[string]string{
		"host_nqn": hostNQN,
	}, http.StatusNoContent, nil)
}

func (c *HTTPClient) DenyHost(ctx context.Context, exportID, hostNQN string) error {
	if strings.TrimSpace(exportID) == "" {
		return fmt.Errorf("export id is required")
	}
	if strings.TrimSpace(hostNQN) == "" {
		return fmt.Errorf("host nqn is required")
	}
	return c.doJSON(ctx, http.MethodPost, "/v1/exports/"+exportID+"/deny-host", map[string]string{
		"host_nqn": hostNQN,
	}, http.StatusNoContent, nil)
}

func (c *HTTPClient) doJSON(ctx context.Context, method, path string, requestBody any, wantStatus int, out any) error {
	var body bytes.Buffer
	if requestBody != nil {
		if err := json.NewEncoder(&body).Encode(requestBody); err != nil {
			return err
		}
	}
	req, err := http.NewRequestWithContext(ctx, method, c.baseURL+path, &body)
	if err != nil {
		return err
	}
	if requestBody != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := c.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != wantStatus {
		var rpcErr struct {
			Error string `json:"error"`
		}
		if err := json.NewDecoder(resp.Body).Decode(&rpcErr); err == nil && rpcErr.Error != "" {
			return fmt.Errorf("exporter http %d: %s", resp.StatusCode, rpcErr.Error)
		}
		return fmt.Errorf("exporter http %d", resp.StatusCode)
	}
	if out == nil || resp.StatusCode == http.StatusNoContent {
		return nil
	}
	return json.NewDecoder(resp.Body).Decode(out)
}
