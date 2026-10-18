// Users panel (host_users.h). Adding and renaming are saved at once; the game shows them the next time it builds its
// user list (its picker, or the next launch). Deleting a save or a user is queued for the next launch (user_admin.h);
// the panel marks queued rows and offers to quit the app so the queue runs when it is opened again.
#import "host_users.h"
#include "user_admin.h"
#include <errno.h>
#include <stdlib.h>

@interface WonderUsersController : UITableViewController
@end

@implementation WonderUsersController {
    wonder_uid users[WONDER_ACCOUNT_MAX_USERS];
    int count;
    BOOL anyPending;
}
- (instancetype)init {
    if (!(self = [super initWithStyle:UITableViewStyleInsetGrouped])) return nil;
    self.title = @"用户与存档";
    return self;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    __weak WonderUsersController *weak = self;
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone primaryAction:
        [UIAction actionWithHandler:^(UIAction *a) { (void)a; [weak dismissViewControllerAnimated:YES completion:nil]; }] menu:nil];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd primaryAction:
        [UIAction actionWithHandler:^(UIAction *a) { (void)a; [weak addUser]; }] menu:nil];
    [self reload];
}
- (void)reload {
    count = wonder_account_list(users, WONDER_ACCOUNT_MAX_USERS);
    if (count > WONDER_ACCOUNT_MAX_USERS) count = WONDER_ACCOUNT_MAX_USERS;
    anyPending = NO; for (int i = 0; i < count; i++) anyPending |= wonder_users_pending(&users[i]) != 0;
    self.navigationItem.rightBarButtonItem.enabled = count < WONDER_ACCOUNT_MAX_USERS;
    [self.tableView reloadData];
}
- (void)fail:(NSString *)what error:(int)error {
    NSString *reason = error == ENOSPC ? @"最多 8 个用户。" : error == EPERM ? @"至少要保留一个用户。" : error == EINVAL ? @"用户名不能为空，且不超过 32 字节（约 10 个汉字）。" : [NSString stringWithFormat:@"错误 %d（%s）", error, strerror(error)];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:what message:reason preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)addUser {
    int error = wonder_account_add(NULL);
    fprintf(stderr, "HOST_USERS add error=%d\n", error);
    if (error) [self fail:@"无法新增用户" error:error];
    [self reload];
}
- (NSString *)nameOf:(const wonder_uid *)uid {
    char name[WONDER_NICKNAME_MAX + 1] = "";
    wonder_account_nickname(uid, name);
    return [NSString stringWithUTF8String:name] ?: @"";
}
- (void)renameUser:(wonder_uid)uid {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"修改用户名" message:nil preferredStyle:UIAlertControllerStyleAlert];
    NSString *current = [self nameOf:&uid];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) { field.text = current; field.clearButtonMode = UITextFieldViewModeAlways; }];
    __weak WonderUsersController *weak = self; __weak UIAlertController *weakAlert = alert;
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"保存" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a) {
        (void)a;
        NSString *text = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        int error = wonder_account_rename(&uid, text.UTF8String);
        fprintf(stderr, "HOST_USERS rename error=%d\n", error);
        if (error) [weak fail:@"无法修改用户名" error:error];
        [weak reload];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)confirm:(NSString *)title message:(NSString *)message button:(NSString *)button then:(void (^)(void))action {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:button style:UIAlertActionStyleDestructive handler:^(UIAlertAction *a) { (void)a; action(); }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)schedule:(wonder_uid)uid flags:(int)flags {
    int error = wonder_users_schedule(&uid, flags);
    fprintf(stderr, "HOST_USERS schedule flags=%d error=%d\n", flags, error);
    if (error) [self fail:@"无法删除" error:error];
    [self reload];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { (void)tableView; return anyPending ? 2 : 1; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section { (void)tableView; return section ? 1 : count; }
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView;
    if (section) return @"退出后从主屏幕重新打开 App，删除会在游戏启动前执行。";
    return @"最多 8 个用户，每个用户一份存档。新增用户和改名立即保存，游戏在重新进入选人界面或下次启动时显示。删除存档、删除用户在下次启动 App 时执行（游戏运行中可能正在使用这些数据）；删除用户会同时删除其存档。";
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    (void)tableView;
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    UIListContentConfiguration *content = UIListContentConfiguration.subtitleCellConfiguration;
    if (path.section) {
        content = UIListContentConfiguration.cellConfiguration;
        content.text = @"退出 App 以执行删除"; content.textProperties.color = UIColor.systemRedColor;
        content.textProperties.alignment = UIListContentTextAlignmentCenter;
        cell.contentConfiguration = content; return cell;
    }
    const wonder_uid *uid = &users[path.row];
    int pending = wonder_users_pending(uid);
    content.text = [self nameOf:uid];
    content.image = [UIImage systemImageNamed:@"person.crop.circle"];
    NSMutableArray *notes = [NSMutableArray array];
    [notes addObject:wonder_users_has_save(uid) ? @"有存档" : @"无存档"];
    if (wonder_account_open_count(uid)) [notes addObject:@"游戏已打开此用户"];
    if (pending & WONDER_PENDING_USER) [notes addObject:@"下次启动删除用户"];
    else if (pending & WONDER_PENDING_SAVE) [notes addObject:@"下次启动删除存档"];
    content.secondaryText = [notes componentsJoinedByString:@" · "];
    if (pending) content.secondaryTextProperties.color = UIColor.systemRedColor;
    cell.contentConfiguration = content;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)path {
    [tableView deselectRowAtIndexPath:path animated:YES];
    if (path.section) {
        [self confirm:@"退出 App？" message:@"退出后从主屏幕重新打开，排队的删除会在游戏启动前执行。" button:@"退出" then:^{
            fprintf(stderr, "HOST_USERS quit to apply pending deletions\n"); exit(0);
        }];
        return;
    }
    wonder_uid uid = users[path.row];
    NSString *name = [self nameOf:&uid];
    int pending = wonder_users_pending(&uid);
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:name message:nil preferredStyle:UIAlertControllerStyleActionSheet];
    __weak WonderUsersController *weak = self;
    [sheet addAction:[UIAlertAction actionWithTitle:@"修改用户名" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a) { (void)a; [weak renameUser:uid]; }]];
    if (pending) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"撤销删除" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a) {
            (void)a; int error = wonder_users_cancel(&uid); if (error) [weak fail:@"无法撤销" error:error]; [weak reload];
        }]];
    }
    if (!(pending & WONDER_PENDING_SAVE) && wonder_users_has_save(&uid)) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"删除存档" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *a) {
            (void)a;
            [weak confirm:[NSString stringWithFormat:@"删除“%@”的存档？", name] message:@"下次启动 App 时删除，删除后无法恢复。" button:@"删除存档" then:^{ [weak schedule:uid flags:WONDER_PENDING_SAVE]; }];
        }]];
    }
    if (!(pending & WONDER_PENDING_USER)) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"删除用户" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *a) {
            (void)a;
            [weak confirm:[NSString stringWithFormat:@"删除用户“%@”？", name] message:@"下次启动 App 时删除，该用户的存档也一并删除，无法恢复。" button:@"删除用户" then:^{ [weak schedule:uid flags:WONDER_PENDING_USER]; }];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    UITableViewCell *cell = [tableView cellForRowAtIndexPath:path];
    sheet.popoverPresentationController.sourceView = cell ?: tableView;
    sheet.popoverPresentationController.sourceRect = cell ? cell.bounds : CGRectZero;
    [self presentViewController:sheet animated:YES completion:nil];
}
@end

UIViewController *wonder_users_panel(void) {
    UINavigationController *navigation = [[UINavigationController alloc] initWithRootViewController:[WonderUsersController new]];
    navigation.modalPresentationStyle = UIModalPresentationFormSheet;
    return navigation;
}
