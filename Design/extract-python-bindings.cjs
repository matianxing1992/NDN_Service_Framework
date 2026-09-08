// Preserve pybind11 declarations without loading the native extension.
const fs=require('fs'),path=require('path');
const base=process.env.NDNSF_TREE_SITTER_ROOT||'/home/tianxing/.codegraph/versions/v1.0.1/lib/node_modules';
const {Parser,Language}=require(path.join(base,'web-tree-sitter/tree-sitter.cjs'));
(async()=>{
 await Parser.init();const p=new Parser();p.setLanguage(await Language.load(path.join(base,'tree-sitter-wasms/out/tree-sitter-cpp.wasm')));
 const result=[];
 for(const file of ['pythonWrapper/src/ndnsf/_ndnsf.cpp','pythonWrapper/src/ndnsf/di_bindings.cpp']){
  const src=fs.readFileSync(file,'utf8'),tree=p.parse(src),entries=[];
  for(const n of tree.rootNode.descendantsOfType('call_expression')){
   const fn=n.childForFieldName('function');if(!fn||fn.type!=='field_expression')continue;
   const op=fn.childForFieldName('field')?.text;
   if(!['def','def_static','def_readwrite','def_readonly','def_property','def_property_readonly','value'].includes(op))continue;
   const args=n.childForFieldName('arguments')?.namedChildren||[];
   if(!args.length||args[0].type!=='string_literal')continue;
   const binding=args[0].text;
   const signatures=args.slice(1).filter(a=>a.type!=='comment').map(a=>{
    if(a.type==='lambda_expression'){const b=a.childForFieldName('body');return a.text.slice(0,b?b.startIndex-a.startIndex:a.text.length).trim()+' { implementation omitted }';}
    return a.text;
   });
   const expression=fn.childForFieldName('argument')?.text||'';
   const owner=expression.match(/py::(?:class_|enum_)<[^;]*?>\s*\(\s*[A-Za-z_][A-Za-z_0-9]*,\s*"([^"]+)"/)?.[1]||expression.match(/^[A-Za-z_][A-Za-z_0-9]*$/)?.[0]||'see-source-owner';
   entries.push({owner,name:binding,operation:op,line:fn.childForFieldName('field').startPosition.row+1,signature:op+'('+binding+',\n'+signatures.join(',\n')+')'});
  }
  const commit=JSON.parse(fs.readFileSync('Design/source-baseline.json','utf8')).baseline_commit;
  const committed=require('child_process').execFileSync('git',['show',commit+':'+file]);
  result.push({file,baseline_commit:commit,matches_commit:committed.equals(Buffer.from(src)),sha256:require('crypto').createHash('sha256').update(src).digest('hex'),entries});tree.delete();
 }
 fs.writeFileSync('Design/api/python-bindings.json',JSON.stringify(result,null,2)+'\n');
 let md='# Python 原生绑定映射\n\n从 pybind11 绑定声明静态提取 Python 名称、C++ 目标或 lambda 参数、py::arg 默认值与调用策略。lambda 实现体省略；构造器 py::init 与动态导出须另查源文件。此表不导入扩展，不声明 ABI 或运行通过。\n';
 for(const f of result){md+='\n## '+f.file+'\n\nSHA-256：`'+f.sha256+'`。\n';for(const e of f.entries)md+='\n### '+e.owner+' · '+e.name+'\n\n[源码](../../'+f.file+'#L'+e.line+')\n\n```cpp\n'+e.signature+'\n```\n';}
 fs.writeFileSync('Design/api/python-bindings.md',md.split('\n').map(line=>line.trimEnd()).join('\n'));
 console.log(JSON.stringify(result.map(f=>({file:f.file,bindings:f.entries.length,unresolved:f.entries.filter(e=>e.owner==='see-source-owner').length}))));
})();
