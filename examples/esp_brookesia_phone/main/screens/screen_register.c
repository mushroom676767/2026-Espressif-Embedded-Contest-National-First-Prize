/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * User registration screen for FOD Smart Tool Cabinet.
 */

#include "screen_register.h"

#include "auth_camera_preview.h"
#include "face_recognition_service.h"
#include "face_model_storage.h"
#include "nfc_manager.h"
#include "register_pin_gate.h"
#include "user_manager.h"
#include "ui_touch_layout.h"
#include "ui_manager.h"
#include "ui_theme.h"
#include "esp_err.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "screen_register";

LV_FONT_DECLARE(register_cjk_font_20);

#define REGISTER_HEADER_H              UI_HEADER_H
#define REGISTER_SESSION_TIMEOUT_MS    60000
#define REGISTER_SESSION_TIMER_MS      1000
#define REGISTER_NAME_MAX_BYTES        64
#define REGISTER_EMPLOYEE_ID_MAX_BYTES 25
#define REGISTER_KB_TOGGLE_TEXT        "中/英"
#define REGISTER_KB_TOGGLE_CHARS       3
#define REGISTER_KB_BTN(width)         (LV_BTNMATRIX_CTRL_POPOVER | (width))

#define REGISTER_CJK_FONT              &register_cjk_font_20

static const char *s_register_kb_map_lc[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    REGISTER_KB_TOGGLE_TEXT, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const char *s_register_kb_map_uc[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    REGISTER_KB_TOGGLE_TEXT, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const char *s_register_kb_map_spec[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    REGISTER_KB_TOGGLE_TEXT, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t s_register_kb_ctrl_lc_map[] = {
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5, REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 6, REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_BTNMATRIX_CTRL_CHECKED | REGISTER_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | REGISTER_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | REGISTER_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | REGISTER_KB_BTN(1),
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BTN_FLAGS | 2
};

static const lv_btnmatrix_ctrl_t s_register_kb_ctrl_uc_map[] = {
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5, REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), REGISTER_KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 6, REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), REGISTER_KB_BTN(3), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_BTNMATRIX_CTRL_CHECKED | REGISTER_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | REGISTER_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | REGISTER_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | REGISTER_KB_BTN(1),
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BTN_FLAGS | 2
};

static const lv_btnmatrix_ctrl_t s_register_kb_ctrl_spec_map[] = {
    REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1),
    REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1), REGISTER_KB_BTN(1),
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BTN_FLAGS | 2
};

#if LV_USE_IME_PINYIN
/* Generated from GB2312 common Chinese characters with heteronym pinyin. */
static lv_pinyin_dict_t s_register_name_pinyin_dict[] = {
    {"a", "啊阿嗄锕呵吖腌"},
    {"ai", "埃挨哎唉哀皑癌蔼矮艾碍爱隘捱嗳嗌嫒瑷暧砹锿霭呆乃奇呃噫"},
    {"an", "鞍氨安俺按暗岸胺案谙埯揞犴庵桉铵鹌黯厂干广盒"},
    {"ang", "肮昂盎仰腌"},
    {"ao", "凹敖熬翱袄傲奥懊澳坳拗嗷岙廒遨媪骜獒聱螯鏊鳌鏖棍嚣噢燠"},
    {"ba", "芭捌扒叭吧笆八疤巴拔跋靶把耙坝霸罢爸茇菝岜灞钯粑鲅魃伯湃萆捭杷"},
    {"bai", "白柏百摆佰败拜稗捭掰擘扒伯啡排派薜呗鞴"},
    {"ban", "斑班搬扳般颁板版扮拌伴瓣半办绊阪坂钣瘢癍舨卑辨彬豳"},
    {"bang", "邦帮梆榜膀绑棒磅蚌镑傍谤蒡浜旁彭螃"},
    {"bao", "苞胞包褒薄雹保堡饱宝抱报暴豹鲍爆勹葆孢煲鸨褓趵龅剥呆刨炮袍曝瀑裒苴"},
    {"bei", "杯碑悲卑北辈背贝钡倍狈备惫焙被孛陂邶蓓呗悖碚鹎褙鐾鞴拔跋臂波葡菩俾埤萆怫蜚"},
    {"ben", "奔苯本笨畚坌贲锛夯体"},
    {"beng", "崩绷甭泵蹦迸嘣甏榜蚌傍旁抨平俸堋唪"},
    {"bi", "逼鼻比鄙笔彼碧蓖蔽毕毙毖币庇痹闭敝弊必壁臂避陛匕俾荜荸萆薜吡哔狴庳愎滗濞弼妣婢嬖璧畀铋秕裨筚箅篦舭襞跸髀卑被辟波肥佛拂幅服虑秘泌脾瞥仳陂陴埤芘捭媲枇檗殍贲馥跛"},
    {"bian", "鞭边编贬扁便变卞辨辩辫遍匾弁苄忭汴缏煸砭碥窆褊蝙笾鳊封拚稹"},
    {"biao", "标彪膘表婊骠杓飑飙飚灬镖镳瘭裱鳔髟苞漂剽嫖鏖"},
    {"bie", "鳖憋别瘪蹩捌扒拔蔽秘撇"},
    {"bin", "彬斌濒滨宾摈傧豳缤玢槟殡膑镔髌鬓份浜"},
    {"bing", "兵冰柄丙秉饼炳病并禀冫邴摒拼平屏枋槟燹"},
    {"bo", "剥玻菠播拨钵波博勃搏铂箔伯帛舶脖膊渤驳卜亳啵饽檗礴钹鹁簸跛踣拔白柏百般薄暴爆泊簿番佛服募怕拍潘跑魄菩蒲瀑孛蕃薜蘖擗溥艴擘趵鲅"},
    {"bu", "捕哺补埠不布步簿部怖卟逋瓿晡钚钸醭薄堡卜附埔拊溥鞴"},
    {"ca", "擦嚓礤蔡拆"},
    {"cai", "猜裁材才财睬踩采彩菜蔡"},
    {"can", "餐参蚕残惭惨灿掺孱骖璨粲黪"},
    {"cang", "苍舱仓沧藏伧臧"},
    {"cao", "操糙槽曹草艹嘈漕螬艚澡造屮"},
    {"ce", "厕策侧册测恻赦栅"},
    {"cen", "岑涔参"},
    {"ceng", "层蹭曾噌僧增"},
    {"cha", "插叉茬茶查碴搽察岔差诧猹馇汊姹杈槎檫锸镲衩接捷刹土斜喳苴荼嚓楂"},
    {"chai", "拆柴豺侪钗瘥虿查差搓茈"},
    {"chan", "搀蝉馋谗缠铲产阐颤冁谄蒇廛忏潺澶羼婵骣觇禅镡蟾躔掺单厘苫兔沾孱嬗"},
    {"chang", "昌猖场尝常偿肠厂敞畅唱倡伥鬯苌菖徜怅惝阊娼嫦昶氅鲳长尚裳倘淌"},
    {"chao", "超抄钞朝嘲潮巢吵炒怊晁焯耖绰剿"},
    {"che", "车扯撤掣彻澈坼屮砗拆池尺斥宅"},
    {"chen", "郴臣辰尘晨忱沉陈趁衬谌谶抻嗔宸琛榇碜龀称橙堪帘沈填湛枕疹伧肜胂眈"},
    {"cheng", "撑称城橙成呈乘程惩澄诚承逞骋秤丞埕枨柽晟塍瞠铖裎蛏酲敞盯净棱盛趟醒郢噌嵊徵樘铛"},
    {"chi", "吃痴持池迟弛驰耻齿侈尺赤翅斥炽傺坻墀茌叱哧啻嗤彳饬媸敕眵鸱瘛褫蚩螭笞篪踟魑茬拆匙哆离莉蛇她抬提拖喜移治郗芪搋沱柢胝眙豉踅"},
    {"chong", "充冲虫崇宠茺忡憧铳舂艟茧酮涌盅种重烛僮潼"},
    {"chou", "抽酬畴踌稠愁筹仇绸瞅丑臭俦帱惆瘳雠扭圳揄溴妯"},
    {"chu", "初出橱厨躇锄雏滁除楚础储矗搐触处畜亍刍怵憷绌杵楮樗褚蜍蹰黜硫柠淑涂絮著助祝"},
    {"chua", "撮"},
    {"chuai", "揣搋啜嘬膪踹啐"},
    {"chuan", "川穿椽传船喘串舛遄巛氚钏舡掾惴踹"},
    {"chuang", "疮窗幢床闯创怆葱囱舂"},
    {"chui", "吹炊捶锤垂椎陲棰槌"},
    {"chun", "春椿醇唇淳纯蠢莼鹑蝽沌肫朐"},
    {"chuo", "戳绰辶辍踔龊躇簇促淖醛荃蔟啜焯斫趵踱"},
    {"ci", "疵茨磁雌辞慈瓷词此刺赐次伺茈呲祠鹚糍差柴措司兹滋嵯蚝螅粢趑"},
    {"cong", "聪葱囱匆从丛苁淙骢琮璁枞窗偬"},
    {"cou", "凑辏腠簇趣奏揍族蔟楱"},
    {"cu", "粗醋簇促蔟徂猝殂酢蹙蹴戚且趣卒槭"},
    {"cuan", "蹿篡窜汆撺爨镩蹲攒"},
    {"cui", "摧崔催脆瘁粹淬翠萃啐悴璀榱毳察洒衰体卒隹"},
    {"cun", "村存寸忖皴蹲浚"},
    {"cuo", "磋撮搓措挫错厝嵯脞锉矬痤鹾蹉差摧昔最瘥"},
    {"da", "搭达答瘩打大耷哒嗒怛妲沓褡笪靼鞑胆迭塌塔疸"},
    {"dai", "呆歹傣戴带殆代贷袋待逮怠埭甙呔岱迨骀绐玳黛大毒隶逯棣"},
    {"dan", "耽担丹单郸掸胆旦氮但惮淡诞弹蛋儋萏啖澹殚赕眈疸瘅聃箪忱憾冉石潭蜒詹湛怛澶檐膻"},
    {"dang", "当挡党荡档谠凼菪宕砀铛裆"},
    {"dao", "刀捣蹈倒岛祷导到稻悼道盗刂叨忉氘焘纛敦受陶佻啁帱惆洮忑"},
    {"de", "德得的锝登底地陟"},
    {"dei", "得"},
    {"deng", "蹬灯登等瞪凳邓噔嶝戥磴镫簦橙澄"},
    {"di", "堤低滴迪敌笛狄涤翟嫡抵底地蒂第帝弟递缔氐籴诋谛邸荻嘀娣柢棣觌砥碲睇镝羝骶逮的隶芍勺提蹄逐坻莜啻杓胝"},
    {"dia", "嗲"},
    {"dian", "颠掂滇碘点典靛垫电佃甸店惦奠淀殿阽坫巅玷钿癜癫簟踮拈涎沾埝蜓"},
    {"diao", "碉叼雕凋刁掉吊钓调铞铫貂鲷稠刀敦鸟挑跳佻倜莜啁蜩踔"},
    {"die", "跌爹碟蝶迭谍叠垤堞揲喋嗲牒瓞耋蹀鲽哆涉踢至窒佚渫褶"},
    {"ding", "丁盯叮钉顶鼎锭定订仃啶玎腚碇铤疔耵酊灯奠汀葶町"},
    {"diu", "丢铥"},
    {"dong", "东冬董懂动栋侗恫冻洞垌咚岽峒氡胨胴硐鸫桐酮筒甬"},
    {"dou", "兜抖斗陡豆逗痘都蔸窦蚪篼读投逾钭窬"},
    {"du", "督毒犊独读堵睹赌杜镀肚度渡妒芏嘟渎椟牍碡蠹笃髑黩都顿土宅橐竺纛"},
    {"duan", "端短锻段断缎椴煅簖踹"},
    {"dui", "堆兑队对怼憝碓镦敦追槌"},
    {"dun", "墩吨蹲敦顿囤钝盾遁沌炖砘礅盹趸俊豚镦"},
    {"duo", "掇哆多夺垛躲朵跺舵剁惰堕咄哚缍柁铎裰踱揣捶度兑隋陀驮杂沱棰酡"},
    {"e", "蛾峨鹅俄额讹娥恶厄扼遏鄂饿噩谔垩苊莪萼呃愕阏屙婀轭腭锇锷鹗颚鳄啊阿隘胺歹蛤哦邑鬲侉叱啐猗庵曷硪疴"},
    {"ei", "诶"},
    {"en", "恩蒽摁"},
    {"er", "而儿耳尔饵洱二贰佴迩珥铒鸸鲕濡"},
    {"fa", "发罚筏伐乏阀法珐垡砝拔泛"},
    {"fan", "藩帆番翻樊矾钒繁凡烦反返范贩犯饭泛蕃蘩幡梵燔畈蹯潘拚袢蟠"},
    {"fang", "坊芳方肪房防妨仿访纺放匚邡枋钫舫鲂彷"},
    {"fei", "菲非啡飞肥匪诽吠肺废沸费芾狒悱淝妃绯榧腓斐扉镄痱蜚篚翡霏鲱拂裴茇怫祓砩"},
    {"fen", "芬酚吩氛分纷坟焚汾粉奋份忿愤粪偾瀵棼鲼鼢扮奔匪盼拚玢燔愍"},
    {"feng", "丰封枫蜂峰锋风疯烽逢冯缝讽奉凤俸酆葑唪沣砜蚌泛方捧逄"},
    {"fo", "佛"},
    {"fou", "否缶不"},
    {"fu", "佛夫敷肤孵扶拂辐幅氟符伏俘服浮涪福袱弗甫抚辅俯釜斧腑府腐赴副覆赋复傅付阜父腹负富讣附妇缚咐匐凫阝郛芙苻茯莩菔拊呋幞怫滏艴孚驸绂绋桴赙祓砩黻黼罘稃馥蚨蜉蝠蝮麸趺跗鲋鳆包哺不沸脯怀还仅莆市芾掊彳溥宓鞴"},
    {"ga", "噶嘎伽尬呷尕尜旮钆胳夹咖轧戛"},
    {"gai", "该改概钙盖溉丐陔垓戤赅骸核芥咳汽胲"},
    {"gan", "干甘杆柑竿肝赶感秆敢赣坩苷尴擀泔淦澉绀橄旰矸疳酐个捍汗奸乾"},
    {"gang", "冈刚钢缸肛纲岗港杠戆罡筻肮扛抗亢伉"},
    {"gao", "篙皋高膏羔糕搞镐稿告睾诰郜藁缟槔槁杲锆浩咎蒿"},
    {"ge", "哥歌搁戈鸽胳疙割革葛格阁隔铬个各咯鬲仡哿圪塥嗝纥搿膈硌镉袼虼舸骼噶盖蛤浩菏合假介可屹砝颌髂"},
    {"gei", "给"},
    {"gen", "根跟亘茛哏艮痕"},
    {"geng", "耕更庚羹埂耿梗哽赓绠鲠颈亢邢硬亘"},
    {"gong", "工攻功恭龚供躬公宫弓巩汞拱贡共廾珙肱蚣觥杠虹红磺咣蛩"},
    {"gou", "钩勾沟苟狗垢构购够佝诟岣遘媾缑枸觏彀笱篝鞲拘句"},
    {"gu", "辜菇咕箍估沽孤姑鼓古蛊骨谷股故顾固雇嘏诂菰呱崮汩梏轱牯牿臌毂瞽罟钴锢鸪鹄痼蛄酤觚鲴鹘皋告滑家贾角枯苦哌胍瓠骰"},
    {"gua", "刮瓜剐寡挂褂卦诖栝胍鸹聒括舌呱惴"},
    {"guai", "乖拐怪掴"},
    {"guan", "棺关官冠观管馆罐惯灌贯倌莞掼涫盥鹳鳏串果纶斡菅矜"},
    {"guang", "光广逛咣犷桄胱横恍潢"},
    {"gui", "瑰规圭硅归龟闺轨鬼诡癸桂柜跪贵刽傀炔匦刿庋宄妫桧晷皈簋鲑鳜概撅祈哇洼娃桅匮隗炅蹶觖"},
    {"gun", "辊滚棍丨衮绲磙鲧混卷"},
    {"guo", "锅郭国果裹过馘埚呙帼崞猓椁虢蜾蝈唬划活涡蠃掴囗聒蜮"},
    {"ha", "蛤哈铪呵虾吓獬"},
    {"hai", "骸孩海氦亥害骇还嗨胲醢咳咴"},
    {"han", "酣憨邯韩含涵寒函喊罕翰撼捍旱憾悍焊汗汉邗菡撖阚瀚晗焓顸颔蚶鼾厂甘感嵌犴泔淦澉旰矸"},
    {"hang", "夯杭航沆绗珩颃肮狠炕吭狼巷行桁酐"},
    {"hao", "壕嚎豪毫郝好耗号浩貉蒿薅嗥嚆濠灏昊皓颢蚝皋镐唬睾妞"},
    {"he", "呵喝荷菏核禾和何合盒阂河涸赫褐鹤贺诃劾壑嗬阖曷盍颌蚵翮格硅哈害貉呼霍揭苛渴洽吓蝎藿哧嗑纥"},
    {"hei", "嘿黑嗨"},
    {"hen", "痕很狠恨掀哏艮"},
    {"heng", "哼亨横衡恒蘅桁行訇珩"},
    {"hng", "哼"},
    {"hong", "轰哄烘虹鸿洪宏弘红黉訇讧荭蕻薨闳泓港共汪洚"},
    {"hou", "喉侯猴吼厚候后堠後逅瘊篌糇鲎骺"},
    {"hu", "呼乎忽瑚壶葫胡蝴狐糊湖弧虎唬护互沪户冱唿囫岵猢怙惚浒滹琥槲轷觳烀煳戽扈祜瓠鹕鹱虍笏醐斛雇核和苦芦戏许羽芴汩胍鹄鹘"},
    {"hua", "花哗华猾滑画划化话骅桦铧敌豁哇找劐侉叱砉稞"},
    {"huai", "槐徊怀淮坏踝划坯圳喟"},
    {"huan", "欢环桓缓换患唤痪豢焕涣宦幻郇奂萑擐圜獾洹浣漶寰逭缳锾鲩鬟灌还皖眩援垸瑗脘"},
    {"huang", "荒慌黄磺蝗簧皇凰惶煌晃幌恍谎隍徨湟潢遑璜肓癀蟥篁鳇横芒茫"},
    {"hui", "灰挥辉徽恢蛔回毁悔慧卉惠晦贿秽会烩汇讳诲绘诙茴荟蕙咴哕喙隳洄浍彗缋珲晖恚虺蟪麾虫堕徊涣溃桧眭睢皓"},
    {"hun", "荤昏婚魂浑混诨馄阍溷棍昆捆珲"},
    {"huo", "豁活伙火获或惑霍货祸劐藿攉嚯夥砉钬锪镬耠蠖扮呵和化越壑灬瓠"},
    {"ji", "击圾基机畸稽积箕肌饥迹激讥鸡姬绩缉吉极棘辑籍集及急疾汲即嫉级挤几脊己蓟技冀季伎祭剂悸济寄寂计记既忌际妓继纪藉丌亟乩剞佶偈诘墼芨芰荠蒺蕺掎叽咭哜唧岌嵴洎彐屐骥畿玑楫殛戟戢赍觊犄齑矶羁嵇稷瘠虮笈笄暨跻跽霁鲚鲫髻麂疵革隔给秸洁居蜡期其棋奇齐系揖倚厝脔郅萁卟猗睽瘵粢瞿"},
    {"jia", "嘉枷夹佳家加荚颊贾甲钾假稼价架驾嫁茄郏葭岬浃迦珈戛胛恝铗镓痂瘕袷蛱笳袈跏咖揩暇夏挟押嘏伽拮呷柙挈骱"},
    {"jian", "歼监坚尖笺间煎兼肩艰奸缄茧检柬碱硷拣捡简俭剪减荐鉴践贱见键箭件健舰剑饯渐溅涧建僭谏谫菅蒹搛囝湔蹇謇缣枧楗戋戬牮犍毽腱睑锏鹣裥笕翦趼踺鲣鞯喊槛沮前浅咸茛犴孱箴"},
    {"jiang", "僵姜将浆江疆蒋桨奖讲匠酱降茳洚绛缰犟礓耩糨豇虹强"},
    {"jiao", "蕉椒礁焦胶交郊浇骄娇搅铰矫侥脚狡角饺缴绞剿教酵轿较叫窖佼僬艽茭挢噍峤徼湫姣敫皎鹪蛟醮跤鲛激嚼觉校妖咬菽叽爝"},
    {"jie", "揭接皆秸街阶截劫节杰捷睫竭洁结解姐戒芥界借介疥诫届讦卩拮喈嗟婕孑桀碣疖颉蚧羯鲒骱差担概亥圾籍家假价桔藉楷渴契拾她祖偕偈诘苴唧獬砝袷暨髻"},
    {"jin", "巾筋斤金今津襟紧锦仅谨进靳晋禁近烬浸尽劲卺荩堇噤馑廑妗缙瑾槿赆觐钅衿矜肋吟湛"},
    {"jing", "荆兢茎睛晶鲸京惊精粳经井警景颈静境敬镜径痉靖竟竞净刭儆阱菁獍憬泾迳弪婧肼胫腈旌靓劲青氏醒檠晟蜻箐"},
    {"jiong", "炯窘冂迥炅扃坷垧"},
    {"jiu", "揪究纠玖韭久灸九酒厩救旧臼舅咎就疚僦啾阄柩桕鸠鹫赳鬏愁噍湫蝤蹴"},
    {"ju", "桔鞠拘狙疽居驹菊局咀矩举沮聚拒据巨具距踞锯俱句惧炬剧倨讵苣苴莒菹掬遽屦琚椐榘榉橘犋飓钜锔窭裾趄醵踽龃雎鞫车仇告拱柜姐且蛆屈渠娶租足佝鄹蘧鬻枸瞿"},
    {"juan", "捐鹃娟倦眷卷绢鄄狷涓桊蠲锩镌隽圈蕊身眩甄擐泫睃蜷"},
    {"jue", "嚼撅攫抉掘倔爵觉决诀绝厥劂谲矍蕨噘噱崛獗孓珏桷橛爝镢蹶觖脚角狂屈蛙穴乙蕞嗟柽梏觳蠼"},
    {"jun", "均菌钧军君峻俊竣浚郡骏捃皲麇龟卷旬匀訇狻睃筠隽"},
    {"ka", "喀咖卡佧咔胩咯"},
    {"kai", "开揩楷凯慨剀垲蒈忾恺铠锎锴喝核渴岂劾溘雉"},
    {"kan", "槛刊堪勘坎砍看侃莰戡龛瞰喊嵌凵阚"},
    {"kang", "康慷糠扛抗亢炕伉闶钪奋杭荒坑沆"},
    {"kao", "考拷烤靠尻栲犒铐搞槁"},
    {"ke", "坷苛柯棵磕颗科壳咳可渴克刻客课嗑岢恪溘骒缂珂轲氪瞌钶锞稞疴窠颏蝌髁呵喀碣盍蚵"},
    {"kei", "刻"},
    {"ken", "肯啃垦恳裉龈狠垠"},
    {"keng", "坑吭铿忐硎"},
    {"kong", "空恐孔控倥崆箜腔穹"},
    {"kou", "抠口扣寇芤蔻叩眍筘挎刳佝彀"},
    {"ku", "枯哭窟苦酷库裤刳堀喾绔骷古掘挎跨圣"},
    {"kua", "夸垮挎跨胯侉髁"},
    {"kuai", "块筷侩快蒯郐哙狯脍会魁傀浍"},
    {"kuan", "宽款髋棵完"},
    {"kuang", "匡筐狂框矿眶旷况诓诳邝圹夼哐纩贶呈逛磺枉兄湟"},
    {"kui", "亏盔岿窥葵奎魁馈愧溃馗匮夔隗蒉揆喹喟悝愦逵暌睽聩蝰篑跬踩傀缺臾觖"},
    {"kun", "坤昆捆困悃阃琨锟醌鲲髡混卵"},
    {"kuo", "括扩廓阔蛞适栝"},
    {"la", "垃拉喇蜡腊辣啦剌邋旯砬瘌蓝落摺"},
    {"lai", "莱来赖崃徕涞濑赉睐铼癞籁黧"},
    {"lan", "蓝婪栏拦篮阑兰澜谰揽览懒缆烂滥岚漤榄斓罱镧褴郴啉懔"},
    {"lang", "琅榔狼廊郎朗浪莨蒗啷阆锒稂螂羹踉"},
    {"lao", "捞劳牢老佬姥酪烙涝潦唠崂栳铑铹痨耢醪撩僚落络蓼獠"},
    {"le", "乐肋了仂叻泐鳓勒嘞"},
    {"lei", "勒雷镭蕾磊累儡垒擂类泪羸诔嘞嫘缧檑耒酹肋漯"},
    {"len", "啉"},
    {"leng", "棱楞冷塄愣"},
    {"li", "厘梨犁黎篱狸离漓理李里鲤礼莉荔吏栗丽厉励砾历利傈例俐痢立粒沥隶力璃哩俪俚郦坜苈莅蓠藜呖唳喱猁溧澧逦娌嫠骊缡枥栎轹戾砺詈罹锂鹂疠疬蛎蜊蠡笠篥粝醴跞雳鲡鳢黧列泣位鬲仂捩叻悝珞砬硌翮霾"},
    {"lia", "俩"},
    {"lian", "联莲连镰廉怜涟帘敛脸链恋炼练蔹奁潋濂琏楝殓臁裢裣蠊鲢零令羸苓搛膦瞵"},
    {"liang", "粮凉梁粱良两辆量晾亮谅墚椋踉魉惊俩莨靓"},
    {"liao", "撩聊僚疗燎寥辽撂镣廖料蓼尥嘹獠寮缭钌鹩佬潦了"},
    {"lie", "列裂烈劣猎冽埒捩咧洌趔躐鬣膊累栗例邋"},
    {"lin", "琳林磷霖临邻鳞淋凛赁吝拎蔺啉嶙廪懔遴檩辚膦瞵粼躏麟任"},
    {"ling", "玲菱零龄铃伶羚凌灵陵岭领另令酃苓呤囹泠绫柃棂瓴聆蛉翎鲮棱冷怜磷拎"},
    {"liu", "溜琉榴硫馏留刘瘤流柳六浏遛骝绺旒熘锍镏鹨鎏泵聊碌陆游蓼泖硐"},
    {"lo", "咯"},
    {"long", "龙聋咙笼窿隆垄拢陇垅茏泷珑栊胧砻癃弄"},
    {"lou", "楼娄搂篓漏陋偻蒌喽嵝镂瘘耧蝼髅牢露"},
    {"lu", "芦卢颅庐炉掳卤虏鲁麓碌露路赂鹿潞禄录陆戮垆撸噜泸渌漉逯璐栌橹轳辂辘氇胪镥鸬鹭簏舻鲈谷角酪六绿蓼瘳"},
    {"luan", "峦挛孪滦卵乱脔娈栾鸾銮"},
    {"lun", "抡轮伦仑沦纶论囵"},
    {"luo", "萝螺罗逻锣箩骡裸落洛骆络倮蠃荦摞猡泺漯珞椤脶镙瘰雒格果咯酪烙碌路捋猓橐硌袼蜾蠡跞"},
    {"lv", "驴吕铝侣旅履屡缕虑氯律率滤绿捋闾榈膂稆褛累鹿偻"},
    {"lve", "掠略锊率"},
    {"m", "呒唔"},
    {"ma", "妈麻玛码蚂马骂嘛吗唛犸嬷杩蟆貉么靡摩抹貊麽"},
    {"mai", "埋买麦卖迈脉劢荬霾哩派咪唛"},
    {"man", "瞒馒蛮满蔓曼慢漫谩墁幔缦熳镘颟螨鳗鞔埋幕"},
    {"mang", "芒茫盲氓忙莽邙漭硭蟒瞢朦"},
    {"mao", "猫茅锚毛矛铆卯茂冒帽貌贸袤茆峁泖瑁昴牦耄旄懋瞀蝥蟊髦耗描牟侔勖蛑"},
    {"me", "么没末麽"},
    {"mei", "玫枚梅酶霉煤没眉媒镁每美昧寐妹媚莓嵋猸浼湄楣镅鹛袂魅糜谜墨某味坶"},
    {"men", "门闷们扪焖懑钔汶鞔"},
    {"meng", "萌蒙檬盟锰猛梦孟勐甍瞢懵朦礞虻蜢蠓艋艨氓明瞑蟒蟊黾"},
    {"mi", "眯醚靡糜迷谜弥米秘觅泌蜜密幂芈冖谧蘼咪嘧猕汨宓弭脒祢敉糸縻麋辟摩溟幺"},
    {"mian", "棉眠绵冕免勉娩缅面沔渑湎宀腼眄冥泯瞑黾"},
    {"miao", "苗描瞄藐秒渺庙妙喵邈缈杪淼眇鹋吵猫缪蜱"},
    {"mie", "蔑灭乜咩蠛篾咪"},
    {"min", "民抿皿敏悯闽苠岷闵泯缗珉愍黾鳘眠汶玟"},
    {"ming", "明螟鸣铭名命冥茗溟暝瞑酩萌盟皿"},
    {"miu", "谬缪"},
    {"mo", "摸摹蘑模膜磨摩魔抹末莫墨默沫漠寞陌谟茉蓦馍嫫殁镆秣瘼耱貊貘麽百佰伯貉嘿脉冒貌么没昧藐帕袜万无勿嬷蟆"},
    {"mou", "谋牟某侔哞缪眸蛑鍪件毋袤厶婺"},
    {"mu", "拇牡亩姆母墓暮幕募慕木目睦牧穆仫坶苜沐毪钼嘿姥模莫牟婺"},
    {"n", "嗯哏哽唔"},
    {"na", "拿哪呐钠那娜纳捺肭镎衲南内絮呶箬"},
    {"nai", "氖乃奶耐奈鼐艿萘柰哪那能佴"},
    {"nan", "南男难喃囡楠腩蝻赧冉囝罱"},
    {"nang", "囊攮囔馕曩"},
    {"nao", "挠脑恼闹淖孬垴呶猱瑙硇铙蛲"},
    {"ne", "呢讷疒哪呐那疔"},
    {"nei", "馁内哪那"},
    {"nen", "嫩恁枘"},
    {"neng", "能而耐"},
    {"ng", "哽唔嗯"},
    {"ni", "妮霓倪泥尼拟你匿腻逆溺伲坭猊怩昵旎睨铌鲵呢嶷祢慝"},
    {"nian", "蔫拈年碾撵捻念辗廿埝辇黏鲇鲶趁粘"},
    {"niang", "娘酿"},
    {"niao", "鸟尿茑嬲脲袅溺尥"},
    {"nie", "捏聂孽啮镊镍涅陧蘖嗫颞臬蹑哪倪泥捻幸乜埝囡"},
    {"nin", "您恁"},
    {"ning", "柠狞凝宁拧泞佞咛甯聍冰泥年攘疑"},
    {"niu", "牛扭钮纽狃忸妞拗蚴"},
    {"nong", "脓浓农弄侬哝咔"},
    {"nou", "耨"},
    {"nu", "奴努怒弩胬孥驽仅褥呶帑肭"},
    {"nuan", "暖濡暧"},
    {"nuo", "挪懦糯诺傩搦喏锘掉哪呐那娜需濡"},
    {"nv", "女恧钕衄絮狃胬"},
    {"nve", "虐疟"},
    {"o", "哦喔噢"},
    {"ou", "欧鸥殴藕呕偶沤讴怄瓯耦区握遇渥"},
    {"pa", "啪趴爬帕怕琶葩杷筢芭扒叭吧把耙派钯"},
    {"pai", "拍排牌徘湃派俳蒎哌啡脾迫"},
    {"pan", "攀潘盘磐盼畔判叛拚爿泮袢襻蟠蹒扳般拌伴半卞番繁胖片彦鄱弁皤"},
    {"pang", "乓庞旁耪胖彷滂逄螃榜膀磅傍方房仿逢彭蒡"},
    {"pao", "抛咆刨炮袍跑泡匏狍庖脬疱苞胞包抱趵"},
    {"pei", "呸胚培裴赔陪配佩沛辔帔旆锫醅霈倍啡肺坏茇掊淠艴妃蜚"},
    {"pen", "喷盆湓吩汾"},
    {"peng", "砰抨烹澎彭蓬棚硼篷膨朋鹏捧碰堋嘭怦蟛榜傍逢亨旁苹庄滂"},
    {"pi", "辟坯砒霹批披劈琵毗啤脾疲皮匹痞僻屁譬丕仳陴邳郫圮埤鼙芘擗噼庀淠媲纰枇甓睥罴铍癖疋蚍蜱貔蚌卑被比庇番否拂副坏培扑俾陂鄱苤萆蕃薜吡帔庳濞痦裨篦"},
    {"pian", "篇偏片骗谝骈犏胼翩蹁扁便辨平缏褊蝙"},
    {"piao", "飘漂瓢票剽嘌嫖缥殍瞟螵膘朴莩骠髟"},
    {"pie", "撇瞥丿苤氕蔽"},
    {"pin", "拼频贫品聘姘嫔榀牝颦泵匕拚娉"},
    {"ping", "乒坪苹萍平凭瓶评屏俜娉枰鲆秤冯砰聘堋"},
    {"po", "泊坡泼颇婆破魄迫粕叵鄱珀钋钷皤笸霸膊番繁剖朴陂泺溥跛"},
    {"pou", "剖裒掊抱部涪培瓿踣"},
    {"pu", "脯扑铺仆莆葡菩蒲埔朴圃普浦谱曝瀑匍噗溥濮璞攴氆攵镤镨蹼剥堡暴卜扶甫苻"},
    {"qi", "期欺栖戚妻七凄漆柒沏其棋奇歧畦崎脐齐旗祈祁骑起岂乞企启契砌器气迄弃汽泣讫亓俟圻芑芪萁萋葺蕲嘁屺岐汔淇骐绮琪琦杞桤槭耆祺憩碛颀蛴蜞綦綮蹊鳍麒甭吃刺抵逗畸稽缉己技伎揭切勤示宿溪枝支吱丌亟偈荠忮忾挈欹恝趿"},
    {"qia", "掐恰洽葜髂卡客咭挈疴袷"},
    {"qian", "牵扦钎铅千迁签仟谦乾黔钱钳前潜遣浅谴堑嵌欠歉倩佥阡凵芊芡茜掮岍悭慊骞搴褰缱椠肷愆钤虔箝柑赶筋纤寨荨撖忏涔湔犍腱"},
    {"qiang", "枪呛腔羌墙蔷强抢丬戕嫱樯戗炝锖锵镪襁蜣羟跄将控哐爿箐跫"},
    {"qiao", "橇锹敲悄桥瞧乔侨巧鞘撬翘峭俏窍劁诮谯荞愀憔缲樵硗跷鞒愁搞蕉焦壳雀捎硝削校招茭峤毳敫醮跤"},
    {"qie", "切且怯窃郄惬妾挈锲箧捷漆沏契砌茄伽蕺唼喋慊渫婕脞趄"},
    {"qin", "钦侵亲秦琴勤芹擒禽寝沁芩揿吣嗪噙溱檎锓螓衾浸堇蓁廑槿衿矜覃"},
    {"qing", "青轻氢倾卿清擎晴氰情顷请庆苘圊檠磬蜻罄箐謦鲭黥精亲声胜倩綮"},
    {"qiong", "琼穷邛芎茕穹蛩筇跫銎鞠"},
    {"qiu", "秋丘邱球求囚酋泅俅巯犰逑遒楸赇虬蚯蝤裘糗鳅鼽仇龟团馗氽邺艽惆愀湫"},
    {"qu", "趋区蛆曲躯屈驱渠取娶龋趣去诎劬蕖蘧岖衢阒璩觑氍朐祛磲鸲癯蛐蠼麴瞿黢鞠巨句蜡戌苣遽枸鞫"},
    {"quan", "圈颧权醛泉全痊拳犬券劝诠荃犭悛绻辁畎铨蜷筌鬈串卷栓拴圳獾桊"},
    {"que", "缺瘸却鹊榷确雀阕阙悫猎屈炔芍攉觳舄"},
    {"qun", "裙群逡蹲遁麇"},
    {"ran", "然燃冉染苒蚺髯"},
    {"rang", "瓤壤攘嚷让禳穰"},
    {"rao", "饶扰绕荛娆桡"},
    {"re", "惹热若偌喏"},
    {"ren", "壬仁人忍韧任认刃妊纫亻仞荏葚饪轫稔衽儿恁"},
    {"reng", "扔仍耳戎艿穰"},
    {"ri", "日"},
    {"rong", "戎茸蓉荣融熔溶容绒冗嵘狨榕肜蝾隔"},
    {"rou", "揉柔肉糅蹂鞣"},
    {"ru", "茹蠕儒孺如辱乳汝入褥蓐薷嚅洳溽濡缛铷襦颥女肉需月"},
    {"ruan", "软阮朊需濡"},
    {"rui", "蕊瑞锐芮蕤枘睿蚋兑内"},
    {"run", "闰润"},
    {"ruo", "若弱偌箬溺惹芮"},
    {"sa", "撒洒萨卅仨挲脎飒蔡檫趿"},
    {"sai", "腮鳃塞赛噻思"},
    {"san", "三叁伞散馓毵糁霰"},
    {"sang", "桑嗓丧搡磉颡"},
    {"sao", "搔骚扫嫂埽缫臊瘙鳋梢哨燥缲"},
    {"se", "瑟色涩啬铯穑泣塞寨槭"},
    {"sen", "森洒"},
    {"seng", "僧"},
    {"sha", "莎砂杀刹沙纱傻啥煞厦唼歃铩痧裟霎鲨哈接杉噎嗄挲"},
    {"shai", "筛晒酾色"},
    {"shan", "珊苫杉山删煽衫闪陕擅赡膳善汕扇缮剡讪鄯埏芟彡潸姗嬗骟膻钐疝蟮舢跚鳝掺单掸邓檀栅儋澹禅髟"},
    {"shang", "墒伤商赏晌上尚裳垧绱殇熵觞汤"},
    {"shao", "梢捎稍烧芍勺韶少哨邵绍劭苕潲蛸筲艄鞘搜削招召溲杓裢笤"},
    {"she", "奢赊蛇舌舍赦摄射慑涉社设厍佘猞滠歙畲麝碟睫拾邪折揲蛞"},
    {"shei", "谁"},
    {"shen", "砷申呻伸身深娠绅神沈审婶甚肾慎渗什诜谂莘哂渖椹胂矧蜃参信湛震葚抻吲糁"},
    {"sheng", "声生甥牲升绳省盛剩胜圣嵊眚笙乘甸姓丞冼垩渑晟媵"},
    {"shi", "匙师失狮施湿诗尸虱十石拾时食蚀实识史矢使屎驶始式示士世柿事拭誓逝势是嗜噬适仕侍释饰氏市恃室视试似谥埘莳蓍弑饣轼贳炻礻铈螫舐筮豉豕鲥鲺堤郝赫舍什斯寺提嘘液肢汁殖峙唑彖耆挈酾"},
    {"shou", "收手首守寿授售受瘦兽扌狩绶艏熟"},
    {"shu", "蔬枢梳殊抒输叔舒淑疏书赎孰熟薯暑曙署蜀黍鼠属术述树束戍竖墅庶数漱恕倏塾菽摅沭澍姝纾毹腧殳秫除娶售嗽透野俞豫朱荼揄涑杼疋"},
    {"shua", "刷耍唰唆涮"},
    {"shuai", "摔衰甩帅蟀率"},
    {"shuan", "栓拴闩涮汕踹"},
    {"shuang", "霜双爽孀泷淙"},
    {"shui", "谁水睡税氵说"},
    {"shun", "吮瞬顺舜盾俊巡恂巛"},
    {"shuo", "说硕朔烁蒴搠妁槊铄勺数嗽溯嗍濯杓"},
    {"si", "斯撕嘶思私司丝死肆寺嗣四饲巳厮兕厶咝汜泗澌姒驷纟缌祀锶鸶耜蛳笥厕食伺似台析已以肄俟菥徙祠雉糸"},
    {"song", "松耸怂颂送宋讼诵凇菘崧嵩忪悚淞竦"},
    {"sou", "搜艘擞嗽叟薮嗖嗾馊溲飕瞍锼螋族涑敕"},
    {"su", "苏酥俗素速粟僳塑溯宿诉肃夙谡蔌嗉愫涑簌觫稣搬缩蓿嗖"},
    {"suan", "酸蒜算狻撰"},
    {"sui", "虽隋随绥髓碎岁穗遂隧祟谇荽濉邃燧眭睢粹尿莎蓑彗"},
    {"sun", "孙损笋荪狲飧榫隼餐跣"},
    {"suo", "蓑梭唆缩琐索锁所唢嗦嗍娑桫睃羧抄霍莎沙衰些逡挲"},
    {"ta", "塌他它她塔獭挞蹋踏闼溻遢榻铊趿鳎搭达哈太拓嗒漯沓"},
    {"tai", "胎苔抬台泰酞太态汰邰薹肽炱钛跆鲐大能呔骀"},
    {"tan", "坍摊贪瘫滩坛檀痰潭谭谈坦毯袒碳探叹炭郯昙忐钽锬覃胆但淡弹沈舔炎湛蕈澹镡"},
    {"tang", "汤塘搪堂棠膛唐糖倘躺淌趟烫傥帑饧溏瑭樘铴镗耥螗螳羰醣惝"},
    {"tao", "掏涛滔绦萄桃逃淘陶讨套鼗啕洮韬饕挑跳姚叨焘"},
    {"te", "特忒忑慝铽匿式"},
    {"tei", "忒"},
    {"teng", "藤腾疼誊滕"},
    {"ti", "梯剔踢锑提题蹄啼体替嚏惕涕剃屉倜荑悌逖绨缇鹈裼醍达堤狄弟是肆折棣睇"},
    {"tian", "天添填田甜恬舔腆掭忝阗殄畋蚕滇典佃甸苫吞沾嗔栝町钿蚺"},
    {"tiao", "挑条迢眺跳佻祧窕蜩笤粜龆鲦髫超稠调桃姚苕啁踔"},
    {"tie", "贴铁帖萜餮蝶占怙"},
    {"ting", "厅听烃汀廷停亭庭挺艇莛葶婷梃町蜓霆奠铤"},
    {"tong", "通桐酮瞳同铜彤童桶捅筒统痛佟僮仝茼嗵恸潼砼侗恫洞恿重垌峒硐艟"},
    {"tou", "偷投头透亠钭骰逗愉"},
    {"tu", "凸秃突图徒途涂屠土吐兔堍荼菟钍酴跌杜余"},
    {"tuan", "湍团抟彖疃揣敦痪税"},
    {"tui", "推颓腿蜕褪退煺弟税脱追忒"},
    {"tun", "吞屯臀氽饨暾豚吨敦囤褪吴逐沌肫炖窀"},
    {"tuo", "拖托脱鸵陀驮驼椭妥拓唾乇佗坨庹沲沱柝橐砣箨酡跎鼍池惰魄蛇税隋他它迤柁铊舄"},
    {"wa", "挖哇蛙洼娃瓦袜佤娲腽凹鞋"},
    {"wai", "歪外崴夭"},
    {"wan", "豌弯湾玩顽丸烷完碗挽晚皖惋宛婉万腕剜芄菀纨绾琬脘畹蜿蔓免娩园莞朊箢"},
    {"wang", "汪王亡枉网往旺望忘妄罔惘辋魍方皇匡芒尢"},
    {"wei", "威巍微危韦违桅围唯惟为潍维苇萎委伟伪尾纬未蔚味畏胃喂魏位渭谓尉慰卫偎诿隈圩葳薇囗帏帷嵬猥猬闱沩洧涠逶娓玮韪軎炜煨痿艉鲔堤机立遗有于倭阢隗芟荽崴猗熨眭睢隹"},
    {"wen", "瘟温蚊文闻纹吻稳紊问刎阌汶玟璺雯昧免娩限眼愠殁笏"},
    {"weng", "嗡翁瓮蓊蕹壅"},
    {"wo", "挝蜗涡窝我斡卧握沃倭莴幄渥肟硪龌瘟夭喔嗌媪杌"},
    {"wu", "巫呜钨乌污诬屋无芜梧吾吴毋武五捂午舞伍侮坞戊雾晤物勿务悟误兀仵阢邬圬芴呒唔庑怃忤浯寤迕妩婺骛杌牾焐鹉鹜痦蜈鋈鼯恶母亡仡侉喔渥於旄瞀蝥笏"},
    {"xi", "昔熙析西硒矽晰嘻吸锡牺稀息希悉膝夕惜熄烯溪汐犀檄袭席习媳喜铣洗系隙戏细僖兮隰郗菥葸蓰奚唏徙饩阋浠淅屣嬉玺樨曦觋欷熹禊禧皙穸蜥螅蟋舄舾羲粞翕醯鼷既腊猎卤栖洒屎撕郄茜咭咦歙嵇裼褶蜴蹊"},
    {"xia", "瞎虾匣霞辖暇峡侠狭下夏吓狎遐瑕柙硖罅黠呼唬假厦斜押呀葭呷哧嗑嗄岈毳歃瘕"},
    {"xian", "掀锨先仙鲜纤咸贤衔舷闲涎弦嫌显险现献县腺馅羡宪陷限线冼苋莶藓岘猃暹娴氙燹祆鹇痫蚬筅籼酰跣跹霰捍肩碱见洒省探铣洗矣彡慊濂寰妗黹锬"},
    {"xiang", "相厢镶香箱襄湘乡翔祥详想响享项巷橡像向象芗葙饷庠骧缃蟓鲞飨亨降攘洋舡"},
    {"xiao", "萧硝霄哮嚣销消宵淆晓小孝校肖啸笑效哓崤潇逍骁绡枭枵筱箫魈号呼唬胶狡俏梢捎芍哨搜削爻佼叟茭咻姣蛸"},
    {"xie", "楔些歇蝎鞋协挟携邪斜胁谐写械卸蟹懈泄泻谢屑偕亵勰燮薤撷獬廨渫瀣邂绁缬榭榍躞溉接桔解契血耶叶豫汁隰苴摺唏喈迦歙眭颉跬鲑骱"},
    {"xin", "薪芯锌欣辛新忻心信衅囟馨忄昕歆鑫款寻莘镡"},
    {"xing", "星腥猩惺兴刑型形邢行醒幸杏性姓陉荇荥擤悻硎省胜研饧"},
    {"xiong", "兄凶胸匈汹雄熊能宪芎"},
    {"xiu", "休修羞朽嗅锈秀袖绣咻岫馐庥溴鸺貅髹臭宿莠煦"},
    {"xu", "墟戌需虚嘘须徐许蓄酗叙旭序恤絮婿绪续吁诩勖蓿洫溆顼栩煦盱胥糈醑呼姐邪休畜于余芋馘圩咻怵浒旮肷朐砉雩"},
    {"xuan", "轩喧宣悬旋玄选癣眩绚儇谖萱揎泫渲漩璇楦暄炫煊碹铉镟痃暖券撰滋亘擐洵涓昕"},
    {"xue", "削靴薛学穴雪血谑泶踅鳕炔哮噱嚯"},
    {"xun", "勋熏循旬询寻驯巡殉汛训讯逊迅巽埙荀荨蕈薰峋徇獯恂洵浔曛窨醺鲟遁荤浚洒梭潭郇狻悛逡鑫"},
    {"ya", "压押鸦鸭呀丫芽牙蚜崖衙涯雅哑亚讶轧伢垭揠吖岈迓娅琊桠氩砑睚痖吾歇邪御札碣疋"},
    {"yan", "焉咽阉烟淹盐严研蜒岩延言颜阎炎沿奄掩眼衍演艳堰燕厌砚雁唁彦焰宴谚验厣赝俨偃兖讠谳郾鄢芫菸崦恹闫湮滟妍嫣琰檐晏胭腌焱罨筵酽魇餍鼹癌俺厂但淡广狠蔫铅涎羡巡殷淫剡阽埏埯揞庵阏洇氤覃趼"},
    {"yang", "殃央鸯秧杨扬佯疡羊洋阳氧仰痒养样漾徉怏泱炀烊恙蛘鞅昂英映"},
    {"yao", "邀腰妖瑶摇尧遥窑谣姚咬舀药要耀钥夭爻吆崾徭幺珧杳轺曜肴鹞窈繇鳐侥么疟陶淫由幼约佻揄崤徼洮瀹铫"},
    {"ye", "椰噎耶爷野冶也页掖业叶曳腋夜液靥谒邺揶晔烨铘喝射墅涂邪斜咽拽荼揲揞洇"},
    {"yi", "一壹医揖铱依伊衣颐夷遗移仪胰疑沂宜姨彝椅蚁倚已乙矣以艺抑易邑屹亿役臆逸肄疫亦裔意毅忆义益溢诣议谊译异翼翌绎刈劓佚佾诒圯埸懿苡薏弈奕挹弋呓咦咿噫峄嶷猗饴怿怡悒漪迤驿缢殪轶贻欹旖熠眙钇镒镱痍瘗癔翊衤蜴舣羿翳酏黟艾怠迭蛾疙姬隶奇崎汽蛇射失施食巳它台叹听尾熙夕羡泄焉噎也印丿仡佗圪坨荑掎搋嗌洫渫犄焱硪雉袂黝"},
    {"yin", "茵荫因殷音阴姻吟银淫寅饮尹引隐印胤鄞廴垠堙茚吲喑狺夤洇氤铟瘾蚓霪币垦潭听烟言壹沂湛众圻芩湮窨龈"},
    {"ying", "英樱婴鹰应缨莹萤营荧蝇迎赢盈影颖硬映嬴郢茔莺萦蓥撄嘤膺滢潆瀛瑛璎楹媵鹦瘿颍罂逞甸景哩央荥哽"},
    {"yo", "哟唷育"},
    {"yong", "拥佣臃痈庸雍踊蛹咏泳涌永恿勇用俑壅墉喁慵邕镛甬鳙饔容遇蕹臾"},
    {"you", "幽优悠忧尤由邮铀犹油游酉有友右佑釉诱又幼卣攸侑莠莜莸尢呦囿宥柚猷牖铕疣蚰蚴蝣鱿黝鼬奥泅扰叹坳揄聱蝤繇"},
    {"yu", "迂淤于盂榆虞愚舆余俞逾鱼愉渝渔隅予娱雨与屿禹宇语羽玉域芋郁遇喻峪御愈欲狱育誉浴寓裕预豫驭禺毓伛俣谀谕萸蓣揄圄圉嵛狳饫馀庾阈鬻妪妤纡瑜昱觎腴欤於煜燠肀聿钰鹆鹬瘐瘀窬窳蜮蝓竽臾舁雩龉奥懊澳谷或亏舒宛王蔚尉梧吾衙吁苑粥圩菸菀拗唷喁噢崛汩栩昙毹腧煨熨蜍"},
    {"yuan", "鸳渊冤元垣袁原援辕园员圆猿源缘远苑愿怨院垸塬掾沅媛瑗橼爰眢鸢螈箢鼋穿捐阮宛咽允芫圜涓畹"},
    {"yue", "曰约越跃岳粤月悦阅龠瀹樾刖钺块乐说蜕钥哕囝妁栎蠖"},
    {"yun", "耘云郧匀陨允运蕴酝晕韵孕郓芸狁恽愠纭韫殒昀氲熨筠盾均宛尉瘟温尹员苑怨菀媪"},
    {"za", "匝砸杂咋拶咂咱扎籴啐嘁"},
    {"zai", "栽哉灾宰载再在仔崽甾才"},
    {"zan", "咱攒暂赞瓒昝簪糌趱錾拶涔湔"},
    {"zang", "赃脏葬奘驵臧藏戕"},
    {"zao", "遭糟凿藻枣早澡蚤躁噪造皂灶燥唣槽草窖"},
    {"ze", "责择则泽仄赜啧帻迮昃笮箦舴侧措咋柞稷"},
    {"zei", "贼"},
    {"zen", "怎谮僭"},
    {"zeng", "增憎赠缯甑罾锃曾综"},
    {"zha", "扎喳渣札铡闸眨栅榨乍炸诈柞揸吒咤哳楂砟痄蚱齄册插查哆蜡轧咋苴喋馇怍渫膪笮"},
    {"zhai", "摘斋宅窄债寨砦瘵侧柴疵翟度祭择膪豸"},
    {"zhan", "瞻毡詹粘沾盏斩崭展蘸栈占战站湛绽谵搌旃颤袒辗湔澶孱醮躔"},
    {"zhang", "长樟章彰漳张掌涨杖丈帐账仗胀瘴障仉鄣幛嶂獐嫜璋蟑"},
    {"zhao", "招昭找沼赵照罩兆肇召爪诏啁棹钊笊朝嘲淖桃蚤著着佻濯晁"},
    {"zhe", "遮折哲蛰辙者锗蔗这浙着谪摺柘辄磔鹧褶蜇赭斥堵庶著乇陬耷嘀褚螫"},
    {"zhei", "这"},
    {"zhen", "珍斟真甄砧臻贞针侦枕疹诊震振镇阵帧圳蓁浈缜桢榛轸赈胗朕祯畛稹鸩箴趁唇滇鼎慎填坫溱椹戡"},
    {"zheng", "蒸挣睁征狰争怔整拯正政症郑证诤峥徵钲铮筝敞承奠丁趟帧丞瞠町鲭"},
    {"zhi", "芝枝支吱蜘知肢脂汁之织职直植殖执值侄址指止趾只旨纸志挚掷至致置帜峙制智秩稚质炙痔滞治窒卮陟郅埴芷摭帙夂忮彘咫骘栉枳栀桎轵轾贽胝膣祉祗黹雉鸷痣蛭絷酯跖踬踯豸觯埃抵积伎祁识示氏恃拓氐徵昵耆砥"},
    {"zhong", "中盅忠钟衷终种肿重仲众冢锺螽舯踵董童夂忪潼蚣舂"},
    {"zhou", "舟周州洲诌粥轴肘帚咒皱宙昼骤荮妯纣绉胄籀酎扭育逐注祝啄侏倜啁喙鬻柚碡舳繇"},
    {"zhu", "珠株蛛朱猪诸诛逐竹烛煮拄瞩嘱主著柱助蛀贮铸筑住注祝驻丶伫侏邾苎茱洙渚潴杼槠橥炷铢疰瘃竺箸舳翥躅麈朝除斗逗柠宁泞属术庶予之茁阻澍楮褚蚰"},
    {"zhua", "抓挝爪"},
    {"zhuai", "拽转"},
    {"zhuan", "专砖转撰赚篆啭馔颛传湍巽沌"},
    {"zhuang", "桩庄装妆撞壮状幢僮奘憧戆艟"},
    {"zhui", "锥追赘坠缀惴骓缒隹揣垂隧致椎萑槌"},
    {"zhun", "谆准肫窀淳敦屯胗盹"},
    {"zhuo", "捉拙卓桌茁酌啄灼浊倬诼擢浞涿濯禚斫镯掇缴淖勺著琢着蕞啜杓棹肫焯箸趵踔躅"},
    {"zi", "兹咨资姿滋淄孜紫籽滓子自渍字谘嵫姊孳缁梓辎赀恣眦锱秭耔笫粢趑觜訾龇鲻髭柴疵次事吱仔茈呲甾"},
    {"zong", "鬃棕踪宗综总纵偬腙粽从枞"},
    {"zou", "邹走奏揍诹陬鄹驺楱鲰趣族"},
    {"zu", "租足卒族祖诅阻组俎镞淬姐沮苴菹啐嘁槭蹴"},
    {"zuan", "钻纂攥缵躜撮赚"},
    {"zui", "嘴醉最罪蕞摧撮堆咀羧觜"},
    {"zun", "尊遵撙樽鳟奠蹲"},
    {"zuo", "琢昨左佐做作坐座阼唑怍胙祚醋撮挫凿乍柞嘬迮砟笮酢"},
    {NULL, NULL},
};
#endif

typedef enum {
    REGISTER_VIEW_PIN = 0,
    REGISTER_VIEW_NAME,
    REGISTER_VIEW_MENU,
    REGISTER_VIEW_FACE_WAIT,
    REGISTER_VIEW_NFC_WAIT,
} register_view_t;

struct screen_register_data_s {
    lv_obj_t *screen;
    lv_obj_t *header;
    lv_obj_t *title_label;
    lv_obj_t *countdown_label;
    lv_obj_t *exit_btn;
    lv_obj_t *exit_btn_label;

    lv_obj_t *pin_layer;
    lv_obj_t *pin_ta;
    lv_obj_t *pin_status_label;
    lv_obj_t *pin_keyboard;

    lv_obj_t *name_layer;
    lv_obj_t *employee_ta;
    lv_obj_t *name_ta;
    lv_obj_t *active_ta;
    lv_obj_t *name_status_label;
    lv_obj_t *keyboard;
    lv_obj_t *pinyin_ime;
    lv_obj_t *pinyin_cand_panel;

    lv_obj_t *menu_layer;
    lv_obj_t *menu_name_label;
    lv_obj_t *edit_btn;

    lv_obj_t *face_layer;
    lv_obj_t *face_canvas;
    lv_obj_t *face_status_label;

    lv_obj_t *nfc_layer;
    lv_obj_t *nfc_status_label;
    lv_obj_t *nfc_hint_label;

    lv_timer_t *session_timer;
    register_view_t active_view;
    bool pinyin_enabled;
    bool waiting;
    bool face_bound;
    bool nfc_bound;
    int32_t remaining_ms;
    char employee_id[REGISTER_EMPLOYEE_ID_MAX_BYTES];
    char name[REGISTER_NAME_MAX_BYTES];
};

static struct screen_register_data_s s_data;

typedef struct {
    face_recognition_event_t event;
} pending_register_face_event_t;

typedef struct {
    nfc_event_t event;
} pending_register_nfc_event_t;

static void show_pin_layer(void);
static void show_name_layer(void);
static void show_menu_layer(void);
static void pin_ok_event_cb(lv_event_t *e);
static void pin_clear_event_cb(lv_event_t *e);
static void start_face_wait(void);
static void start_nfc_wait(void);
static void apply_register_keyboard_map(void);
static void reset_pinyin_state(void);

static bool has_non_space(const char *text)
{
    if (text == NULL) {
        return false;
    }

    while (*text != '\0') {
        if (!isspace((unsigned char)*text)) {
            return true;
        }
        text++;
    }
    return false;
}

static void set_status_text(const char *text, lv_color_t color)
{
    if (s_data.name_status_label == NULL) {
        return;
    }

    lv_label_set_text(s_data.name_status_label, text != NULL ? text : "");
    lv_obj_set_style_text_color(s_data.name_status_label, color, LV_PART_MAIN);
}

static bool validate_identity_inputs(bool save)
{
    const char *employee_id = lv_textarea_get_text(s_data.employee_ta);
    const char *name = lv_textarea_get_text(s_data.name_ta);

    if (!has_non_space(employee_id)) {
        set_status_text("请输入员工编号", UI_COLOR_DANGER);
        return false;
    }

    if (!user_manager_is_valid_employee_id(employee_id)) {
        set_status_text("员工编号只能包含字母/数字/_ 或 -", UI_COLOR_DANGER);
        return false;
    }

    if (!has_non_space(name)) {
        set_status_text("请输入用户姓名", UI_COLOR_DANGER);
        return false;
    }

    if (save) {
        snprintf(s_data.employee_id, sizeof(s_data.employee_id), "%s", employee_id);
        snprintf(s_data.name, sizeof(s_data.name), "%s", name);
        esp_err_t ret = user_manager_upsert_identity(s_data.employee_id, s_data.name);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Save user identity failed: %s", esp_err_to_name(ret));
            set_status_text("保存用户身份失败", UI_COLOR_DANGER);
            return false;
        }
    }

    return true;
}

static void update_countdown_label(void)
{
    if (s_data.countdown_label == NULL) {
        return;
    }

    if (!s_data.waiting) {
        lv_label_set_text(s_data.countdown_label, "");
        return;
    }

    int32_t seconds = (s_data.remaining_ms + 999) / 1000;
    if (seconds < 0) {
        seconds = 0;
    }
    lv_label_set_text_fmt(s_data.countdown_label, "%02lds", (long)seconds);
}

static void hide_all_layers(void)
{
    if (s_data.pin_layer != NULL) {
        lv_obj_add_flag(s_data.pin_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_data.name_layer != NULL) {
        lv_obj_add_flag(s_data.name_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_data.menu_layer != NULL) {
        lv_obj_add_flag(s_data.menu_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_data.edit_btn != NULL) {
        lv_obj_add_flag(s_data.edit_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_data.face_layer != NULL) {
        lv_obj_add_flag(s_data.face_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_data.nfc_layer != NULL) {
        lv_obj_add_flag(s_data.nfc_layer, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_exit_button_text(const char *text)
{
    if (s_data.exit_btn_label != NULL) {
        lv_label_set_text(s_data.exit_btn_label, text);
        lv_obj_center(s_data.exit_btn_label);
    }
}

static void apply_register_keyboard_map(void)
{
    if (s_data.keyboard == NULL) {
        return;
    }

    lv_keyboard_mode_t mode = lv_keyboard_get_mode(s_data.keyboard);
    if (mode == LV_KEYBOARD_MODE_TEXT_UPPER) {
        lv_btnmatrix_set_map(s_data.keyboard, s_register_kb_map_uc);
        lv_btnmatrix_set_ctrl_map(s_data.keyboard, s_register_kb_ctrl_uc_map);
    } else if (mode == LV_KEYBOARD_MODE_SPECIAL) {
        lv_btnmatrix_set_map(s_data.keyboard, s_register_kb_map_spec);
        lv_btnmatrix_set_ctrl_map(s_data.keyboard, s_register_kb_ctrl_spec_map);
    } else {
        lv_btnmatrix_set_map(s_data.keyboard, s_register_kb_map_lc);
        lv_btnmatrix_set_ctrl_map(s_data.keyboard, s_register_kb_ctrl_lc_map);
    }
}

static void reset_pinyin_state(void)
{
#if LV_USE_IME_PINYIN
    if (s_data.pinyin_ime == NULL) {
        return;
    }

    lv_ime_pinyin_t *ime = (lv_ime_pinyin_t *)s_data.pinyin_ime;
    ime->ta_count = 0;
    ime->cand_num = 0;
    ime->py_page = 0;
    memset(ime->input_char, 0, sizeof(ime->input_char));

    if (s_data.pinyin_cand_panel != NULL) {
        lv_obj_add_flag(s_data.pinyin_cand_panel, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

static void remove_keyboard_toggle_text(void)
{
    lv_obj_t *textarea = s_data.active_ta != NULL ? s_data.active_ta : s_data.name_ta;
    if (textarea == NULL) {
        return;
    }

    for (int i = 0; i < REGISTER_KB_TOGGLE_CHARS; i++) {
        lv_textarea_del_char(textarea);
    }
}

static void stop_wait(bool return_to_menu)
{
    bool was_face = s_data.waiting && s_data.active_view == REGISTER_VIEW_FACE_WAIT;
    bool was_nfc = s_data.waiting && s_data.active_view == REGISTER_VIEW_NFC_WAIT;

    s_data.waiting = false;
    s_data.remaining_ms = 0;
    update_countdown_label();

    if (s_data.session_timer != NULL) {
        lv_timer_pause(s_data.session_timer);
    }

    if (was_face) {
        face_recognition_cancel();
        auth_camera_preview_stop();
    }
    if (was_nfc) {
        nfc_manager_set_event_callback(NULL, NULL);
    }

    if (return_to_menu) {
        show_menu_layer();
    }
}

static void set_face_status_text(const char *text)
{
    if (s_data.face_status_label == NULL) {
        return;
    }

    lv_label_set_text(s_data.face_status_label, text != NULL ? text : "");
    lv_obj_clear_flag(s_data.face_status_label, LV_OBJ_FLAG_HIDDEN);
}

static void set_nfc_status_text(const char *text, lv_color_t color)
{
    if (s_data.nfc_status_label == NULL) {
        return;
    }

    lv_label_set_text(s_data.nfc_status_label, text != NULL ? text : "");
    lv_obj_set_style_text_color(s_data.nfc_status_label, color, LV_PART_MAIN);
}

static void finish_wait(const char *reason)
{
    ESP_LOGI(TAG, "Register wait finished: %s", reason);
    stop_wait(true);
}

static void session_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!s_data.waiting) {
        return;
    }

    s_data.remaining_ms -= REGISTER_SESSION_TIMER_MS;
    update_countdown_label();

    if (s_data.remaining_ms <= 0) {
        finish_wait("timeout");
    }
}

static void exit_btn_event_cb(lv_event_t *e)
{
    (void)e;

    if (s_data.waiting) {
        finish_wait("user exit");
        return;
    }

    stop_wait(false);
    memset(s_data.employee_id, 0, sizeof(s_data.employee_id));
    memset(s_data.name, 0, sizeof(s_data.name));
    ui_manager_goto_screen(SCREEN_STANDBY);
}

static void screen_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_UNLOADED) {
        stop_wait(false);
        memset(s_data.employee_id, 0, sizeof(s_data.employee_id));
        memset(s_data.name, 0, sizeof(s_data.name));
        show_pin_layer();
    }
}

static void identity_ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target(e);

    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        s_data.active_ta = target;
        lv_keyboard_set_textarea(s_data.keyboard, target);
        if (target == s_data.name_ta) {
            s_data.pinyin_enabled = true;
            lv_keyboard_set_mode(s_data.keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
            apply_register_keyboard_map();
#if LV_USE_IME_PINYIN
            if (s_data.pinyin_cand_panel != NULL) {
                lv_obj_align_to(s_data.pinyin_cand_panel, s_data.keyboard, LV_ALIGN_OUT_TOP_MID, 0, -8);
                lv_obj_move_foreground(s_data.pinyin_cand_panel);
            }
#endif
        } else {
            s_data.pinyin_enabled = false;
            lv_keyboard_set_mode(s_data.keyboard, LV_KEYBOARD_MODE_TEXT_UPPER);
            apply_register_keyboard_map();
            reset_pinyin_state();
        }
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED) {
        set_status_text("", UI_COLOR_DANGER);
    }
}

static void name_keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        uint16_t btn_id = lv_btnmatrix_get_selected_btn(s_data.keyboard);
        if (btn_id == LV_BTNMATRIX_BTN_NONE) {
            return;
        }

        const char *text = lv_btnmatrix_get_btn_text(s_data.keyboard, btn_id);
        if (text == NULL) {
            return;
        }

        if (strcmp(text, REGISTER_KB_TOGGLE_TEXT) == 0) {
            remove_keyboard_toggle_text();
            s_data.pinyin_enabled = !s_data.pinyin_enabled;
            reset_pinyin_state();
            apply_register_keyboard_map();
            return;
        }

        apply_register_keyboard_map();

        if (!s_data.pinyin_enabled) {
            reset_pinyin_state();
        } else if (s_data.pinyin_cand_panel != NULL) {
            lv_obj_align_to(s_data.pinyin_cand_panel, s_data.keyboard, LV_ALIGN_OUT_TOP_MID, 0, -8);
            lv_obj_move_foreground(s_data.pinyin_cand_panel);
        }
        return;
    }

    if (code == LV_EVENT_READY) {
        if (validate_identity_inputs(true)) {
            show_menu_layer();
        }
    }
}

static void next_btn_event_cb(lv_event_t *e)
{
    (void)e;
    if (validate_identity_inputs(true)) {
        show_menu_layer();
    }
}

static void edit_name_btn_event_cb(lv_event_t *e)
{
    (void)e;
    show_name_layer();
}

static void face_bind_btn_event_cb(lv_event_t *e)
{
    (void)e;
    start_face_wait();
}

static void nfc_bind_btn_event_cb(lv_event_t *e)
{
    (void)e;
    start_nfc_wait();
}

static void register_face_async_cb(void *user_data)
{
    pending_register_face_event_t *pending = (pending_register_face_event_t *)user_data;
    if (pending == NULL) {
        return;
    }

    face_recognition_event_t event = pending->event;
    free(pending);

    if (!s_data.waiting || s_data.active_view != REGISTER_VIEW_FACE_WAIT) {
        return;
    }

    switch (event.type) {
        case FACE_REC_EVT_NO_FACE:
            set_face_status_text("未检测到人脸");
            break;
        case FACE_REC_EVT_MULTI_FACE:
            set_face_status_text("请保持画面中只有一张人脸");
            break;
        case FACE_REC_EVT_ENROLL_PROGRESS:
            lv_label_set_text_fmt(s_data.face_status_label,
                                  "已保存人脸样本 %u/3",
                                  event.sample_count);
            lv_obj_clear_flag(s_data.face_status_label, LV_OBJ_FLAG_HIDDEN);
            break;
        case FACE_REC_EVT_ENROLL_DONE:
            s_data.face_bound = true;
            set_face_status_text("人脸绑定完成");
            stop_wait(true);
            break;
        case FACE_REC_EVT_ERROR:
            set_face_status_text(event.message[0] != '\0' ? event.message : "人脸绑定失败");
            break;
        default:
            break;
    }
}

static void register_face_event_cb(const face_recognition_event_t *event, void *user_ctx)
{
    (void)user_ctx;

    if (event == NULL) {
        return;
    }

    pending_register_face_event_t *pending =
        (pending_register_face_event_t *)calloc(1, sizeof(*pending));
    if (pending == NULL) {
        ESP_LOGW(TAG, "No memory to queue face register event");
        return;
    }
    pending->event = *event;

    esp_err_t lock_ret = esp_lv_adapter_lock(pdMS_TO_TICKS(100));
    if (lock_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to lock LVGL for face register event: %s", esp_err_to_name(lock_ret));
        free(pending);
        return;
    }

    lv_res_t async_ret = lv_async_call(register_face_async_cb, pending);
    esp_lv_adapter_unlock();

    if (async_ret != LV_RES_OK) {
        ESP_LOGW(TAG, "Failed to queue face register event");
        free(pending);
    }
}

static void show_name_layer(void)
{
    stop_wait(false);
    hide_all_layers();

    s_data.active_view = REGISTER_VIEW_NAME;
    s_data.pinyin_enabled = true;
    lv_label_set_text(s_data.title_label, "用户注册");
    set_exit_button_text(LV_SYMBOL_CLOSE " 取消");
    set_status_text("", UI_COLOR_DANGER);
    lv_obj_clear_flag(s_data.name_layer, LV_OBJ_FLAG_HIDDEN);
    s_data.active_ta = s_data.employee_ta;
    lv_keyboard_set_textarea(s_data.keyboard, s_data.employee_ta);
    lv_keyboard_set_mode(s_data.keyboard, LV_KEYBOARD_MODE_TEXT_UPPER);
    s_data.pinyin_enabled = false;
    apply_register_keyboard_map();
    reset_pinyin_state();

    if (s_data.edit_btn != NULL) {
        lv_obj_add_flag(s_data.edit_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void register_nfc_async_cb(void *user_data)
{
    pending_register_nfc_event_t *pending = (pending_register_nfc_event_t *)user_data;
    if (pending == NULL) {
        return;
    }

    nfc_event_t event = pending->event;
    free(pending);

    if (!s_data.waiting || s_data.active_view != REGISTER_VIEW_NFC_WAIT) {
        nfc_manager_ack_event();
        return;
    }

    set_nfc_status_text("已读取NFC卡，正在保存", UI_COLOR_WARNING);
    if (s_data.nfc_hint_label != NULL) {
        lv_label_set_text_fmt(s_data.nfc_hint_label, "UID：%s（%u字节）",
                              event.card_hex, (unsigned)event.raw_len);
    }

    esp_err_t ret = user_manager_set_nfc(s_data.employee_id, event.card_hex);
    if (ret == ESP_OK) {
        ret = nfc_manager_save_card_binding(event.card_hex, s_data.employee_id, s_data.name);
    }
    if (ret == ESP_OK) {
        s_data.nfc_bound = true;
        set_nfc_status_text("PN532卡绑定完成", UI_COLOR_SUCCESS);
        stop_wait(true);
        nfc_manager_ack_event();
    } else {
        ESP_LOGE(TAG, "NFC card bind failed: %s", esp_err_to_name(ret));
        set_nfc_status_text("保存失败，PN532继续轮询", UI_COLOR_DANGER);
        if (s_data.nfc_hint_label != NULL) {
            lv_label_set_text_fmt(s_data.nfc_hint_label,
                                  "UID：%s\n请重新贴卡，或退出后检查用户信息",
                                  event.card_hex);
        }
        nfc_manager_ack_event();
    }
}

static void register_nfc_event_cb(const nfc_event_t *event, void *user_ctx)
{
    (void)user_ctx;

    if (event == NULL) {
        return;
    }

    pending_register_nfc_event_t *pending =
        (pending_register_nfc_event_t *)calloc(1, sizeof(*pending));
    if (pending == NULL) {
        ESP_LOGW(TAG, "No memory to queue NFC register event");
        nfc_manager_ack_event();
        return;
    }
    pending->event = *event;

    esp_err_t lock_ret = esp_lv_adapter_lock(pdMS_TO_TICKS(100));
    if (lock_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to lock LVGL for NFC register event: %s", esp_err_to_name(lock_ret));
        free(pending);
        nfc_manager_ack_event();
        return;
    }

    lv_res_t async_ret = lv_async_call(register_nfc_async_cb, pending);
    esp_lv_adapter_unlock();

    if (async_ret != LV_RES_OK) {
        ESP_LOGW(TAG, "Failed to queue NFC register event");
        free(pending);
        nfc_manager_ack_event();
    }
}

static void show_pin_layer(void)
{
    stop_wait(false);
    hide_all_layers();

    s_data.active_view = REGISTER_VIEW_PIN;
    lv_label_set_text(s_data.title_label, "注册验证");
    set_exit_button_text(LV_SYMBOL_CLOSE " 取消");
    lv_textarea_set_text(s_data.pin_ta, "");
    lv_label_set_text(s_data.pin_status_label, "");
    lv_obj_clear_flag(s_data.pin_layer, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_data.pin_keyboard, s_data.pin_ta);
}

static void pin_ok_event_cb(lv_event_t *e)
{
    (void)e;
    const char *pin = lv_textarea_get_text(s_data.pin_ta);
    if (register_pin_gate_is_valid(pin)) {
        lv_textarea_set_text(s_data.pin_ta, "");
        lv_label_set_text(s_data.pin_status_label, "");
        show_name_layer();
        return;
    }

    lv_label_set_text(s_data.pin_status_label, "PIN码错误");
    lv_obj_set_style_text_color(s_data.pin_status_label, UI_COLOR_DANGER, LV_PART_MAIN);
}

static void pin_clear_event_cb(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(s_data.pin_ta, "");
    lv_label_set_text(s_data.pin_status_label, "");
}

static void show_menu_layer(void)
{
    stop_wait(false);
    hide_all_layers();

    s_data.active_view = REGISTER_VIEW_MENU;
    lv_label_set_text(s_data.title_label, "绑定用户身份");
    set_exit_button_text(LV_SYMBOL_CLOSE " 取消");

    if (s_data.menu_name_label != NULL) {
        lv_label_set_text_fmt(s_data.menu_name_label,
                              "用户:%s  %s\n人脸:%s    NFC:%s",
                              s_data.employee_id,
                              s_data.name,
                              s_data.face_bound ? "已绑定" : "未绑定",
                              s_data.nfc_bound ? "已绑定" : "未绑定");
    }

    lv_obj_clear_flag(s_data.menu_layer, LV_OBJ_FLAG_HIDDEN);
    if (s_data.edit_btn != NULL) {
        lv_obj_clear_flag(s_data.edit_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void start_wait_common(register_view_t view, const char *title)
{
    stop_wait(false);
    hide_all_layers();

    s_data.active_view = view;
    s_data.waiting = true;
    s_data.remaining_ms = REGISTER_SESSION_TIMEOUT_MS;
    lv_label_set_text(s_data.title_label, title);
    set_exit_button_text(LV_SYMBOL_CLOSE " Exit");

    update_countdown_label();
    lv_timer_reset(s_data.session_timer);
    lv_timer_resume(s_data.session_timer);
}

static void start_face_wait(void)
{
    start_wait_common(REGISTER_VIEW_FACE_WAIT, "绑定人脸");
    lv_label_set_text(s_data.face_status_label, "正在启动摄像头...");
    lv_obj_clear_flag(s_data.face_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_data.face_layer, LV_OBJ_FLAG_HIDDEN);

    if (!face_model_storage_is_ready()) {
        lv_label_set_text_fmt(s_data.face_status_label,
                              "人脸模型不可用: %s",
                              face_model_storage_last_error());
    } else if (auth_camera_preview_start(s_data.face_canvas, s_data.face_status_label) != ESP_OK) {
        lv_label_set_text(s_data.face_status_label, "摄像头不可用");
        stop_wait(false);
    } else {
        esp_err_t ret = face_recognition_start_enroll(s_data.employee_id, s_data.name,
                                                      register_face_event_cb, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Start face enroll failed: %s", esp_err_to_name(ret));
            set_face_status_text("人脸绑定不可用");
            stop_wait(false);
        }
    }
}

static void start_nfc_wait(void)
{
    ESP_LOGI(TAG, "Starting NFC register wait session");
    start_wait_common(REGISTER_VIEW_NFC_WAIT, "绑定PN532 NFC卡");
    lv_obj_clear_flag(s_data.nfc_layer, LV_OBJ_FLAG_HIDDEN);

    if (!nfc_manager_is_ready()) {
        set_nfc_status_text("PN532读卡器不可用", UI_COLOR_DANGER);
        lv_label_set_text(s_data.nfc_hint_label,
                          "请检查供电、UART接线和HSU模式，然后重新启动设备");
        stop_wait(false);
        return;
    }

    set_nfc_status_text("PN532正在轮询卡片", UI_COLOR_TEXT_PRIMARY);
    lv_label_set_text(s_data.nfc_hint_label,
                      "请将NFC卡贴近读卡器并保持不动\n检测到卡片后将自动保存UID");
    nfc_manager_flush_input();
    nfc_manager_set_event_callback(register_nfc_event_cb, NULL);
}

static lv_obj_t *create_header_button(lv_obj_t *parent)
{
    lv_obj_t *btn = ui_create_btn_secondary_large(parent);
    lv_obj_set_size(btn, 146, UI_BTN_H_MD);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_center(label);
    s_data.exit_btn_label = label;
    lv_obj_add_event_cb(btn, exit_btn_event_cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static lv_obj_t *create_action_button(lv_obj_t *parent, const char *icon_text,
                                      const char *title, const char *hint,
                                      lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 290, 170);
    lv_obj_set_style_bg_color(btn, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, UI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, color, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x00000040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_darken(UI_COLOR_BG_PANEL, 20),
                              LV_PART_MAIN | LV_STATE_PRESSED);

    if (ui_touch_is_damaged_mode()) {
        lv_obj_set_style_pad_all(btn, UI_PADDING_SM, LV_PART_MAIN);

        lv_obj_t *icon = lv_label_create(btn);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 16, -10);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_36, LV_PART_MAIN);
        lv_obj_set_style_text_color(icon, color, LV_PART_MAIN);
        lv_label_set_text(icon, icon_text);

        lv_obj_t *title_label = lv_label_create(btn);
        lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 68, -12);
        lv_obj_set_style_text_font(title_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
        lv_obj_set_style_text_color(title_label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
        lv_label_set_text(title_label, title);

        lv_obj_t *hint_label = lv_label_create(btn);
        lv_obj_set_width(hint_label, UI_TOUCH_SAFE_CONTENT_W - 82);
        lv_obj_align(hint_label, LV_ALIGN_LEFT_MID, 68, 16);
        lv_obj_set_style_text_font(hint_label, UI_FONT_STATUS, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
        lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_label_set_text(hint_label, hint);

        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
        return btn;
    }

    lv_obj_t *icon = lv_label_create(btn);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, color, LV_PART_MAIN);
    lv_label_set_text(icon, icon_text);

    lv_obj_t *title_label = lv_label_create(btn);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_text_font(title_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(title_label, title);

    lv_obj_t *hint_label = lv_label_create(btn);
    lv_obj_set_width(hint_label, 240);
    lv_obj_align(hint_label, LV_ALIGN_TOP_MID, 0, 122);
    lv_obj_set_style_text_font(hint_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint_label, hint);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void create_header(void)
{
    s_data.header = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.header, UI_SCREEN_WIDTH, REGISTER_HEADER_H);
    lv_obj_align(s_data.header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.header, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.header, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_data.header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.header, LV_OBJ_FLAG_SCROLLABLE);

    s_data.countdown_label = lv_label_create(s_data.header);
    lv_obj_set_width(s_data.countdown_label, 120);
    lv_obj_align(s_data.countdown_label, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_set_style_text_font(s_data.countdown_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.countdown_label, UI_COLOR_WARNING, LV_PART_MAIN);
    lv_label_set_text(s_data.countdown_label, "");

    s_data.title_label = lv_label_create(s_data.header);
    lv_obj_align(s_data.title_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(s_data.title_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.title_label, UI_COLOR_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(s_data.title_label, "用户注册");

    s_data.exit_btn = create_header_button(s_data.header);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(s_data.exit_btn, UI_TOUCH_SAFE_PAD, 8, UI_TOUCH_SAFE_CONTENT_W, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(s_data.exit_btn, 146, UI_BTN_H_MD);
        lv_obj_align(s_data.exit_btn, LV_ALIGN_RIGHT_MID, -18, 0);
    }
}

static void create_pin_layer(void)
{
    s_data.pin_layer = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.pin_layer, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - REGISTER_HEADER_H);
    lv_obj_align(s_data.pin_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.pin_layer, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.pin_layer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.pin_layer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(s_data.pin_layer);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_text_font(label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(label, "请输入注册PIN码");

    s_data.pin_ta = lv_textarea_create(s_data.pin_layer);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(s_data.pin_ta, UI_TOUCH_SAFE_PAD, 72, UI_TOUCH_SAFE_CONTENT_W, 56);
    } else {
        lv_obj_set_size(s_data.pin_ta, 420, 64);
        lv_obj_align(s_data.pin_ta, LV_ALIGN_TOP_MID, 0, 82);
    }
    lv_textarea_set_one_line(s_data.pin_ta, true);
    lv_textarea_set_password_mode(s_data.pin_ta, true);
    lv_textarea_set_max_length(s_data.pin_ta, 12);
    lv_textarea_set_placeholder_text(s_data.pin_ta, "注册PIN码");
    lv_obj_set_style_text_font(s_data.pin_ta, UI_FONT_BODY_BOLD, LV_PART_MAIN);

    lv_obj_t *ok_btn = ui_create_btn_primary_large(s_data.pin_layer);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(ok_btn, 0, 460, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(ok_btn, 170, UI_BTN_H_MD);
        lv_obj_align(ok_btn, LV_ALIGN_TOP_MID, -96, 164);
    }
    lv_obj_add_event_cb(ok_btn, pin_ok_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_obj_set_style_text_font(ok_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(ok_label, LV_SYMBOL_OK " 进入");
    lv_obj_center(ok_label);

    lv_obj_t *clear_btn = ui_create_btn_secondary_large(s_data.pin_layer);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(clear_btn, 1, 460, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(clear_btn, 170, UI_BTN_H_MD);
        lv_obj_align(clear_btn, LV_ALIGN_TOP_MID, 96, 164);
    }
    lv_obj_add_event_cb(clear_btn, pin_clear_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_label = lv_label_create(clear_btn);
    lv_obj_set_style_text_font(clear_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(clear_label, "清除");
    lv_obj_center(clear_label);

    s_data.pin_status_label = lv_label_create(s_data.pin_layer);
    lv_obj_set_width(s_data.pin_status_label, 480);
    lv_obj_align(s_data.pin_status_label, LV_ALIGN_TOP_MID, 0, 230);
    lv_obj_set_style_text_font(s_data.pin_status_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.pin_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_data.pin_status_label, "");

    s_data.pin_keyboard = lv_keyboard_create(s_data.pin_layer);
    if (ui_touch_is_damaged_mode()) {
        lv_obj_set_size(s_data.pin_keyboard, UI_TOUCH_SAFE_CONTENT_W, 300);
        lv_obj_align(s_data.pin_keyboard, LV_ALIGN_TOP_LEFT, UI_TOUCH_SAFE_PAD, 150);
    } else {
        lv_obj_set_size(s_data.pin_keyboard, 520, 230);
        lv_obj_align(s_data.pin_keyboard, LV_ALIGN_BOTTOM_MID, 0, -16);
    }
    lv_keyboard_set_mode(s_data.pin_keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(s_data.pin_keyboard, s_data.pin_ta);

    lv_obj_add_flag(s_data.pin_layer, LV_OBJ_FLAG_HIDDEN);
}

static void create_name_layer(void)
{
    s_data.name_layer = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.name_layer, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - REGISTER_HEADER_H);
    lv_obj_align(s_data.name_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.name_layer, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.name_layer, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.name_layer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.name_layer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(s_data.name_layer);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_text_font(label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(label, "请输入员工编号和用户姓名");

    s_data.employee_ta = lv_textarea_create(s_data.name_layer);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(s_data.employee_ta, UI_TOUCH_SAFE_PAD, 50, UI_TOUCH_SAFE_CONTENT_W, 52);
    } else {
        lv_obj_set_size(s_data.employee_ta, 680, 58);
        lv_obj_align(s_data.employee_ta, LV_ALIGN_TOP_MID, 0, 72);
    }
    lv_textarea_set_one_line(s_data.employee_ta, true);
    lv_textarea_set_max_length(s_data.employee_ta, REGISTER_EMPLOYEE_ID_MAX_BYTES - 1);
    lv_textarea_set_placeholder_text(s_data.employee_ta, "员工编号");
    lv_obj_set_style_text_font(s_data.employee_ta, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_add_event_cb(s_data.employee_ta, identity_ta_event_cb, LV_EVENT_ALL, NULL);

    s_data.name_ta = lv_textarea_create(s_data.name_layer);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(s_data.name_ta, UI_TOUCH_SAFE_PAD, 112, UI_TOUCH_SAFE_CONTENT_W, 52);
    } else {
        lv_obj_set_size(s_data.name_ta, 680, 58);
        lv_obj_align(s_data.name_ta, LV_ALIGN_TOP_MID, 0, 144);
    }
    lv_textarea_set_one_line(s_data.name_ta, true);
    lv_textarea_set_max_length(s_data.name_ta, REGISTER_NAME_MAX_BYTES - 1);
    lv_textarea_set_placeholder_text(s_data.name_ta, "姓名或拼音");
    lv_obj_set_style_text_font(s_data.name_ta, REGISTER_CJK_FONT, LV_PART_MAIN);
    lv_obj_add_event_cb(s_data.name_ta, identity_ta_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *next_btn = ui_create_btn_primary_large(s_data.name_layer);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(next_btn, 0, 176, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(next_btn, 210, UI_BTN_H_MD);
        lv_obj_align(next_btn, LV_ALIGN_TOP_MID, 0, 218);
    }
    lv_obj_add_event_cb(next_btn, next_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_label = lv_label_create(next_btn);
    lv_obj_set_style_text_font(next_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(next_label, LV_SYMBOL_OK " 下一步");
    lv_obj_center(next_label);

    s_data.name_status_label = lv_label_create(s_data.name_layer);
    lv_obj_set_width(s_data.name_status_label, 620);
    lv_obj_align(s_data.name_status_label, LV_ALIGN_TOP_MID, 0, 262);
    lv_obj_set_style_text_font(s_data.name_status_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.name_status_label, UI_COLOR_DANGER, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.name_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_data.name_status_label, "");

    s_data.keyboard = lv_keyboard_create(s_data.name_layer);
    if (ui_touch_is_damaged_mode()) {
        lv_obj_set_size(s_data.keyboard, UI_TOUCH_SAFE_CONTENT_W, 280);
        lv_obj_align(s_data.keyboard, LV_ALIGN_TOP_LEFT, UI_TOUCH_SAFE_PAD, 242);
    } else {
        lv_obj_set_size(s_data.keyboard, 760, 230);
        lv_obj_align(s_data.keyboard, LV_ALIGN_BOTTOM_MID, 0, -16);
    }
    lv_obj_set_style_text_font(s_data.keyboard, REGISTER_CJK_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_data.keyboard, REGISTER_CJK_FONT, LV_PART_ITEMS);
    lv_keyboard_set_mode(s_data.keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    apply_register_keyboard_map();
    s_data.active_ta = s_data.employee_ta;
    lv_keyboard_set_textarea(s_data.keyboard, s_data.employee_ta);

#if LV_USE_IME_PINYIN
    s_data.pinyin_ime = lv_ime_pinyin_create(s_data.name_layer);
    lv_obj_set_style_text_font(s_data.pinyin_ime, REGISTER_CJK_FONT, LV_PART_MAIN);
    lv_ime_pinyin_set_keyboard(s_data.pinyin_ime, s_data.keyboard);
    lv_ime_pinyin_set_mode(s_data.pinyin_ime, LV_IME_PINYIN_MODE_K26);
    lv_ime_pinyin_t *ime = (lv_ime_pinyin_t *)s_data.pinyin_ime;
    memset(ime->py_num, 0, sizeof(ime->py_num));
    memset(ime->py_pos, 0, sizeof(ime->py_pos));
    lv_ime_pinyin_set_dict(s_data.pinyin_ime, s_register_name_pinyin_dict);

    s_data.pinyin_cand_panel = lv_ime_pinyin_get_cand_panel(s_data.pinyin_ime);
    lv_obj_set_parent(s_data.pinyin_cand_panel, s_data.name_layer);
    if (ui_touch_is_damaged_mode()) {
        lv_obj_set_size(s_data.pinyin_cand_panel, UI_TOUCH_SAFE_CONTENT_W, 44);
        lv_obj_align_to(s_data.pinyin_cand_panel, s_data.keyboard, LV_ALIGN_OUT_TOP_LEFT, 0, -8);
    } else {
        lv_obj_set_size(s_data.pinyin_cand_panel, 760, 44);
        lv_obj_align_to(s_data.pinyin_cand_panel, s_data.keyboard, LV_ALIGN_OUT_TOP_MID, 0, -8);
    }
    lv_obj_set_style_bg_color(s_data.pinyin_cand_panel, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_data.pinyin_cand_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.pinyin_cand_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_data.pinyin_cand_panel, UI_COLOR_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.pinyin_cand_panel, UI_RADIUS_SM, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_data.pinyin_cand_panel, REGISTER_CJK_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_data.pinyin_cand_panel, REGISTER_CJK_FONT, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_data.pinyin_cand_panel, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.pinyin_cand_panel, UI_COLOR_TEXT_PRIMARY, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_data.pinyin_cand_panel, UI_COLOR_BG_DARK, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_move_foreground(s_data.pinyin_cand_panel);
#endif

    lv_obj_add_event_cb(s_data.keyboard, name_keyboard_event_cb, LV_EVENT_ALL, NULL);
}

static void create_menu_layer(void)
{
    s_data.menu_layer = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.menu_layer, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - REGISTER_HEADER_H);
    lv_obj_align(s_data.menu_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.menu_layer, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.menu_layer, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.menu_layer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.menu_layer, LV_OBJ_FLAG_SCROLLABLE);

    s_data.menu_name_label = lv_label_create(s_data.menu_layer);
    lv_obj_set_width(s_data.menu_name_label, 720);
    lv_obj_align(s_data.menu_name_label, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_text_font(s_data.menu_name_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.menu_name_label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.menu_name_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_data.menu_name_label, "用户:");

    lv_obj_t *face_btn = create_action_button(
        s_data.menu_layer, LV_SYMBOL_EYE_OPEN, "绑定人脸",
        "启动摄像头预览", UI_COLOR_SUCCESS, face_bind_btn_event_cb);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(face_btn, 0, 120, 104);
    } else {
        lv_obj_align(face_btn, LV_ALIGN_CENTER, -160, 40);
    }

    lv_obj_t *nfc_btn = create_action_button(
        s_data.menu_layer, LV_SYMBOL_OK, "绑定NFC",
        "等待NFC卡", UI_COLOR_PRIMARY, nfc_bind_btn_event_cb);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(nfc_btn, 1, 120, 104);
    } else {
        lv_obj_align(nfc_btn, LV_ALIGN_CENTER, 160, 40);
    }

    lv_obj_t *edit_parent = ui_touch_is_damaged_mode() ? s_data.screen : s_data.menu_layer;
    s_data.edit_btn = ui_create_btn_secondary_large(edit_parent);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(s_data.edit_btn, 0, 492, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(s_data.edit_btn, 210, UI_BTN_H_MD);
        lv_obj_align(s_data.edit_btn, LV_ALIGN_BOTTOM_MID, 0, -36);
    }
    lv_obj_add_event_cb(s_data.edit_btn, edit_name_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *edit_label = lv_label_create(s_data.edit_btn);
    lv_obj_set_style_text_font(edit_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(edit_label, LV_SYMBOL_EDIT " 编辑姓名");
    lv_obj_center(edit_label);

    if (ui_touch_is_damaged_mode()) {
        lv_obj_add_flag(s_data.edit_btn, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(s_data.menu_layer, LV_OBJ_FLAG_HIDDEN);
}

static void create_face_layer(void)
{
    s_data.face_layer = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.face_layer, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - REGISTER_HEADER_H);
    lv_obj_align(s_data.face_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.face_layer, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.face_layer, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.face_layer, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_data.face_layer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.face_layer, LV_OBJ_FLAG_SCROLLABLE);

    s_data.face_canvas = lv_canvas_create(s_data.face_layer);
    lv_obj_align(s_data.face_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_data.face_canvas, LV_OBJ_FLAG_SCROLLABLE);

    s_data.face_status_label = lv_label_create(s_data.face_layer);
    lv_obj_set_width(s_data.face_status_label, 760);
    lv_obj_align(s_data.face_status_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(s_data.face_status_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.face_status_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.face_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_data.face_status_label, "正在启动摄像头...");

    lv_obj_t *hint = lv_label_create(s_data.face_layer);
    lv_obj_set_width(hint, 760);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_text_font(hint, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint, "请正对摄像头绑定此用户");

    lv_obj_add_flag(s_data.face_layer, LV_OBJ_FLAG_HIDDEN);
}

static void create_nfc_layer(void)
{
    s_data.nfc_layer = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.nfc_layer, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - REGISTER_HEADER_H);
    lv_obj_align(s_data.nfc_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_data.nfc_layer, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.nfc_layer, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.nfc_layer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_data.nfc_layer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(s_data.nfc_layer);
    lv_obj_set_size(panel, 600, 340);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(panel, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, UI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, UI_COLOR_PRIMARY_DIM, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(panel);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, UI_COLOR_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(icon, LV_SYMBOL_REFRESH);

    s_data.nfc_status_label = lv_label_create(panel);
    lv_obj_set_width(s_data.nfc_status_label, 520);
    lv_obj_align(s_data.nfc_status_label, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_set_style_text_font(s_data.nfc_status_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.nfc_status_label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.nfc_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_data.nfc_status_label, "PN532正在轮询卡片");

    s_data.nfc_hint_label = lv_label_create(panel);
    lv_obj_set_width(s_data.nfc_hint_label, 520);
    lv_obj_align(s_data.nfc_hint_label, LV_ALIGN_TOP_MID, 0, 184);
    lv_obj_set_style_text_font(s_data.nfc_hint_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.nfc_hint_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.nfc_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_data.nfc_hint_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_data.nfc_hint_label,
                      "请将NFC卡贴近读卡器并保持不动\n检测到卡片后将自动保存UID");

    lv_obj_add_flag(s_data.nfc_layer, LV_OBJ_FLAG_HIDDEN);
}

void screen_register_create(void)
{
    ESP_LOGI(TAG, "Creating register screen...");
    memset(&s_data, 0, sizeof(s_data));

    s_data.screen = lv_obj_create(NULL);
    lv_obj_add_style(s_data.screen, (lv_style_t *)ui_style_get_bg(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_data.screen, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_size(s_data.screen, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_clear_flag(s_data.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_data.screen, screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    create_header();
    create_pin_layer();
    create_name_layer();
    create_menu_layer();
    create_face_layer();
    create_nfc_layer();

    s_data.session_timer = lv_timer_create(session_timer_cb, REGISTER_SESSION_TIMER_MS, NULL);
    lv_timer_pause(s_data.session_timer);

    show_pin_layer();
    ESP_LOGI(TAG, "Register screen created");
}

lv_obj_t *screen_register_get_screen(void)
{
    return s_data.screen;
}

void screen_register_start(void)
{
    stop_wait(false);
    memset(s_data.employee_id, 0, sizeof(s_data.employee_id));
    memset(s_data.name, 0, sizeof(s_data.name));
    s_data.face_bound = false;
    s_data.nfc_bound = false;
    lv_textarea_set_text(s_data.pin_ta, "");
    lv_label_set_text(s_data.pin_status_label, "");
    lv_textarea_set_text(s_data.employee_ta, "");
    lv_textarea_set_text(s_data.name_ta, "");
    show_pin_layer();
}

void screen_register_cancel_wait(void)
{
    if (s_data.waiting) {
        finish_wait("cancel");
    }
}

struct screen_register_data_s *screen_register_get_data(void)
{
    return &s_data;
}
