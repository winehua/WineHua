import { UIAbility } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { GameLibraryService } from '../service/GameLibraryService';

// Owns only the library window. Closing it never tears down a Wine session.
export default class GameLibraryAbility extends UIAbility {
  onWindowStageCreate(stage: window.WindowStage): void {
    GameLibraryService.getInstance().init(this.context);
    const main = stage.getMainWindowSync();
    main.setWindowTitle('久鱼 · 游戏库').catch((err: Error) => {
      hilog.warn(0, 'GameLibrary', 'Window title: %{public}s', JSON.stringify(err));
    });
    stage.loadContent('pages/GameLibrary', (err) => {
      if (err.code) hilog.error(0, 'GameLibrary', 'Load library failed: %{public}s', JSON.stringify(err));
    });
  }

  onDestroy(): void {
    GameLibraryService.getInstance().cancelScan();
  }
}
