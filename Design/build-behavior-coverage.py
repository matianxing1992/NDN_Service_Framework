"""Map every captured function to an explicit, conservative review status."""
from pathlib import Path
import json
root=Path(__file__).resolve().parent
inventory=json.loads((root/'api/inventory.json').read_text())
contracts=json.loads((root/'api/contract-map.json').read_text())
mapping={}
for card in contracts:
    for key in card['api_ids']: mapping.setdefault(key,[]).append(card['contract'])
raw=[dict(api_id=e['id'], file=f['file'], name=e['name'], line=e['line'],
              status='CONTRACT_REFERENCED' if e['id'] in mapping else 'SIGNATURE_ONLY',
              contracts=mapping.get(e['id'],[]))
         for f in inventory['files'] for e in f['entries'] if e['kind']=='function']
grouped={}
for entry in raw:
    line=entry.pop('line')
    grouped.setdefault(entry['api_id'], dict(entry, declaration_lines=[]))['declaration_lines'].append(line)
entries=list(grouped.values())
(root/'api/behavior-coverage.json').write_text(json.dumps(dict(
    note='Contract references are not complete semantic or runtime qualification.',
    entries=entries),ensure_ascii=False,indent=2)+'\n')
print('Behavior coverage:',len(entries),'functions')
