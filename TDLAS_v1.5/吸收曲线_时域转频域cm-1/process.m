%
clc; close all; clear all;




fils = dir("*.lvm");
size(fils)
filN = fils(2).name
dat = textread(filN);
size(dat);
%dat(1:10,:)
for i = 1:length(fils);
    filN = fils(i).name;
    dat = textread(filN);
%    figure(101);
%    subplot(2,3,i); plot(dat(:,2)); grid on
    datt{i} = dat(:,2);
end
% figure()
% plot(datt{2});

figure(101); 
subplot(3,1,1); plot(datt{1}); hold on
plot(datt{2});
xx(:,1) = datt{1}; xx(:,2) = datt{2};  %datt{1}为吸收信号，datt{2}为标准具信号
yy = xx(110000:280000,:); %去上升斜面
hold off; grid on;
plot(yy)
x = yy(:,2); % x为标准具曲线
y = yy(:,1); % y为吸收曲线
%  findpeaks 找到峰位置
%'MinPeakHeight:指定峰值的最小高度。只有高度超过此阈值的峰值才会被找到。默认值为-inf，表示没有高度阈值。
%'MinPeakDistance:指定峰值之间的最小距离。如果两个峰值之间的距离小于此值，只有其中一个峰值会被保留。默认值为1。
tt = length(x);
t = (1:tt)';
[vp,pp] = findpeaks(-x,'MinPeakDistance',3000,'MinPeakHeight',-1);  %vp峰值的高度向量，pp峰值位置向量
hold on; plot(t(pp),x(pp),'k*'); grid on; % #鏍囧嚭鎵?鏈夌殑鏍囧噯鍏峰嘲鍊肩偣锛屾煡楠屾槸鍚︽湁璇紒锛侊紒
size(pp)
t1 = t(pp); v1 = (1:length(pp))';  
% v1=v1.*.08761./30
pp = polyfit(t1,v1,3);
v = polyval(pp,t);
xlabel('数组序列')


subplot(3,1,2);  plot(v,x); grid on; hold on;  plot(v,y);
sta1 = 1; stp1 = 40000; sta2 =120001; stp2 = tt;
ta = [t(sta1:stp1); t(sta2:stp2)];
ya = [y(sta1:stp1); y(sta2:stp2)];
ppp = polyfit(ta,ya,5);
y0 = polyval(ppp,t);
plot(v,y0,'k');
alp = log(y0./y);
% 将x轴坐标与频率对准，光纤标准具的fsr=0.8761GHz，频率GHz与波数cm-1的转化为 1f（GHz）=30*f(cm-1)
[v_f index_f ]=max(alp)
v_2f=(v-v(index_f)).*0.8761./30 +6534.365  %最大值的位置为置为0，并偏置到峰中心高度处


subplot(3,1,3); plot(v_2f,alp); grid on; hold on;
absorb_data=importdata('[2]C2H2,HITRAN_ X = 0.0005, T = 300 K, P = 0.75 atm, L = 100 cm .csv');
absorb_data=absorb_data.data;
plot(absorb_data(:,1),absorb_data(:,2));
xlabel('波数cm-1')
ylabel('吸收系数')