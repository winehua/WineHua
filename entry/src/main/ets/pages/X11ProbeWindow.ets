import { HgsWindowSurface } from 'hgs';

@Entry({ useSharedStorage: true })
@Component
struct X11ProbeWindow {
  @LocalStorageProp('hgsWindowId') windowId: string = '';
  @LocalStorageProp('hgsEpoch') epoch: string = '';

  build() {
    Stack() {
      if (this.windowId.length > 0 && this.epoch.length > 0) {
        HgsWindowSurface({ windowId: this.windowId, epoch: this.epoch, interactive: true })
      }
    }.width('100%').height('100%')
  }
}
