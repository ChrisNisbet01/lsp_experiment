const path = require('path');
const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

let client;

function activate(context) {
  const config = vscode.workspace.getConfiguration('toyLsp');
  let serverPath = config.get('serverPath', '');

  if (!serverPath) {
    // Default: binary is at ../build/src/toy_lsp relative to this extension
    serverPath = path.resolve(__dirname, '..', 'build', 'src', 'toy_lsp');
  }

  const serverOptions = {
    command: serverPath,
    args: [],
  };

  const clientOptions = {
    documentSelector: [{ scheme: 'file', language: 'toylang' }],
    trace: 'verbose'
  };

  client = new LanguageClient('toyLsp', 'Toy LSP', serverOptions, clientOptions);
  context.subscriptions.push(client.start());
}

function deactivate() {
  if (client) {
    return client.stop();
  }
}

module.exports = { activate, deactivate };
