// Extract declared interface surfaces; this is syntax inventory, not ABI analysis.
const fs = require('fs');
const path = require('path');
const base = process.env.NDNSF_TREE_SITTER_ROOT || '/home/tianxing/.codegraph/versions/v1.0.1/lib/node_modules';
const {Parser, Language} = require(path.join(base, 'web-tree-sitter/tree-sitter.cjs'));
(async () => {
  await Parser.init();
  const parser = new Parser();
  parser.setLanguage(await Language.load(path.join(base, 'tree-sitter-wasms/out/tree-sitter-cpp.wasm')));
  const output = [];
  for (const file of JSON.parse(fs.readFileSync(0, 'utf8'))) {
    const source = fs.readFileSync(file, 'utf8');
    const tree = parser.parse(source);
    const entries = [], errors = [];
    function visit(node, scope = [], access = 'public') {
      if (node.type === 'ERROR' || node.isMissing) errors.push({line: node.startPosition.row + 1, text: node.text.slice(0, 120)});
      if (['namespace_definition', 'class_specifier', 'struct_specifier', 'enum_specifier'].includes(node.type)) {
        const name = node.childForFieldName('name')?.text || '(anonymous)';
        const body = node.childForFieldName('body');
        if (node.type !== 'namespace_definition' && access !== 'private') {
          const prefix = body ? source.slice(node.startIndex, body.startIndex).trim() : node.text;
          entries.push({name: [...scope,name].join('::'), kind: node.type, access, line: node.startPosition.row+1, signature: prefix, fields: []});
        }
        if (body) {
          let a = node.type === 'class_specifier' ? 'private' : 'public';
          for (const c of body.namedChildren) {
            if (c.type === 'access_specifier') { a = c.text; continue; }
            visit(c, [...scope,name], access === 'private' ? 'private' : a);
          }
        }
        return;
      }
      if (['function_definition','declaration','field_declaration','alias_declaration','type_definition','enumerator'].includes(node.type)) {
        if (access === 'private') return;
        // Nested class definitions remain independent entries.
        for (const c of node.namedChildren) if (['class_specifier','struct_specifier','enum_specifier'].includes(c.type)) { visit(c,scope,access); return; }
        const body = node.childForFieldName('body');
        let signature = (body ? source.slice(node.startIndex,body.startIndex) : node.text).trim();
        const init = node.namedChildren.find(c=>c.type==='field_initializer_list');
        if (init) signature=source.slice(node.startIndex,init.startIndex).trim();
        if (node.parent?.type === 'template_declaration') signature=source.slice(node.parent.startIndex,node.startIndex).trim()+'\n'+signature;
        const decl = node.childForFieldName('declarator') || node.childForFieldName('name');
        const funcs = node.descendantsOfType('function_declarator');
        const func = funcs[0];
        let name = func?.childForFieldName('declarator')?.text || decl?.text || signature.split(/\s+/).slice(-1)[0];
        if (!name || signature.startsWith('friend ')) return;
        let prior=node.previousNamedSibling;
        const comments=[];
        while(prior?.type==='comment') {comments.unshift(prior.text);prior=prior.previousNamedSibling;}
        entries.push({name:[...scope,name].join('::'),kind:func?'function':node.type,access,line:node.startPosition.row+1,signature,documentation:comments.join('\n'),test_only:/ForTest|TestAccess|LocalMock|TestPort/.test(name+signature)});
        return;
      }
      for(const c of node.namedChildren) visit(c,scope,access);
    }
    visit(tree.rootNode);
    output.push({file,entries,parse_errors:errors});
    tree.delete();
  }
  console.log(JSON.stringify(output));
})();
