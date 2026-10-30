// Both the file browser and game library submit the same native launch contract.
// An empty environment intentionally preserves the native, game-specific CPU
// and graphics policies; it must not be replaced with a generic CPU preset.
export interface WineProgramOptions {
  windowsExePath: string;
  argv: string[];
  environment: Record<string, string>;
  workingDirectory: string;
  d3dBackend: string;
  dxvkBackend: string;
  openglRenderer: string;
}

export function wineProgramOptions(executable: string, directory: string,
  backend: string, dxvk: string, argv: string[] = [],
  environment: Record<string, string> = {}, openglRenderer: string = 'auto'): WineProgramOptions {
  const selected = backend === 'dxvk_legacy' || backend === 'dxvk_legacy_d3d9' ||
    backend === 'dxvk_modern_2_6' || backend === 'vkd3d_limited_500k' ? backend : 'wined3d';
  return { windowsExePath: executable, workingDirectory: directory, argv, environment,
    openglRenderer: openglRenderer === 'zink' || openglRenderer === 'virpipe' ? openglRenderer : 'auto',
    d3dBackend: selected,
    dxvkBackend: selected === 'dxvk_modern_2_6' ? 'dxvk_modern_2_6' :
      (selected.startsWith('dxvk_') ? 'dxvk_legacy' :
        (dxvk === 'dxvk_modern_2_6' ? 'dxvk_modern_2_6' : 'dxvk_legacy')) };
}
