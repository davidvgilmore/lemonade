import React, { useEffect, useRef, useState } from 'react';
import { serverFetch } from '../utils/serverConfig';
import type { RoutingPolicyDoc } from '../utils/decisionTree';

interface Props { policy: RoutingPolicyDoc | null; unavailableReason: string | null; }

const ArcRouterTestPanel: React.FC<Props> = ({ policy, unavailableReason }) => {
  const [request, setRequest] = useState<Record<string, unknown> | null>(null);
  const [filename, setFilename] = useState('');
  const [result, setResult] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const generation = useRef(0);
  useEffect(() => { generation.current++; setResult(null); setError(null); }, [policy]);
  useEffect(() => () => { generation.current++; }, []);
  const test = async () => {
    const current = ++generation.current;
    setBusy(true); setError(null); setResult(null);
    try {
      const response = await serverFetch('/routing/validate', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ policy, arc_request: request }),
      });
      const data = await response.json();
      if (current !== generation.current) return;
      if (!response.ok) throw new Error(typeof data.error === 'string' ? data.error : `Local decision failed (${response.status}).`);
      if (typeof data.decision?.route_to !== 'string') throw new Error('Local runtime returned no destination.');
      setResult(data.decision.route_to);
    } catch (failure) {
      if (current === generation.current) setError(failure instanceof Error ? failure.message : 'Local decision failed.');
    } finally { setBusy(false); }
  };
  return <div className="settings-content custom-collection-content"><div className="form-section">
    <label className="form-label">Test an ARC Conversation</label>
    <p className="settings-description">Import a decision request exported by your ARC runtime. It includes the conversation and routing context. This test selects a destination without sending anything to that model.</p>
    <input type="file" accept=".json,application/json" aria-label="Import ARC test conversation" disabled={busy} onChange={async event => {
      const file = event.target.files?.[0]; event.target.value = '';
      if (!file) return;
      generation.current++;
      setRequest(null); setFilename(''); setResult(null); setError(null);
      try {
        const value = JSON.parse(await file.text());
        if (!value || Array.isArray(value) || value.schema_version !== 'rayline.arc.policy-decision-request.v1') throw new Error('Choose an ARC decision request exported by your runtime.');
        setRequest(value); setFilename(file.name);
      } catch (failure) { setError(failure instanceof Error ? failure.message : 'Could not read conversation.'); }
    }} />
    {filename && <p className="settings-description">{filename}</p>}
    {unavailableReason && <p role="status">{unavailableReason}</p>}
    <button type="button" className="settings-save-button" disabled={!policy || !request || busy} onClick={test}>{busy ? 'Testing…' : 'Test Local Decision'}</button>
    {result && <p role="status">Selected destination: <strong>{result}</strong></p>}
    {error && <p role="alert">{error}</p>}
  </div></div>;
};
export default ArcRouterTestPanel;
