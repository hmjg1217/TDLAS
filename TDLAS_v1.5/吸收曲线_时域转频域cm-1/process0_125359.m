%
clc; close all; clear all;
fils = dir("*.lvm");
tic
for i = 1:length(fils);
    filN = fils(i).name;
    dat = textread(filN);
%    figure(100);
%    subplot(2,3,i); plot(dat(:,2)); grid on
    datt{i} = dat(:,2);
end

xx(:,1) = datt{1}; xx(:,2) = datt{2};   yy = xx(110000:280000,:);
tim0 = clock;
x = yy(:,2);    y = yy(:,1);    tt = length(x); t = (1:tt)';
[vp,pv] = findpeaks(-x,'MinPeakDistance',3000,'MinPeakHeight',-1);
size(pv);   t1 = t(pv); v1 = (1:length(pv))';
pp1 = polyfit(t1,v1,3);  v = polyval(pp1,t);
sta1 = 1; stp1 = 40000; sta2 =120001; stp2 = tt;
ta = [t(sta1:stp1); t(sta2:stp2)];  ya = [y(sta1:stp1); y(sta2:stp2)];
pp2 = polyfit(ta,ya,5); y0 = polyval(pp2,t);    alp = log(y0./y);

par0 = [18.5,0.3,3,2];    %fv = vvoigtshape(par0,v);   【峰中心位置，峰高度 高斯部分FWHM 洛伦兹部分FWHM】
ppar = lsqcurvefit(@vvoigtshape,par0,v,alp),    fv1 = vvoigtshape(ppar,v);
[mean(abs(alp-fv1)),std(abs(alp-fv1))]  %计算残差均值和残差的标准差
tim1 = clock;   etime(tim1,tim0) % 计算程序运行时间

figure(101); subplot(3,1,1); %plot(datt{1}); hold on; plot(datt{2}); grid on;
    hold off; plot(yy);   hold on; plot(t(pv),x(pv),'k*'); grid on; 
    subplot(3,1,2);  plot(v,x); grid on; hold on;  plot(v,y);plot(v,y0,'k');
    subplot(3,1,3); plot(v,alp); grid on; 
figure(110); subplot(2,1,1); plot(v,alp); hold on; grid on; plot(v,fv1,'r');
    subplot(4,1,3); plot(v,alp-fv1); grid on;
    
    
    figure(); 
    subplot(2,1,1),plot(v,alp); hold on; grid on; plot(v,fv1,'r');
    subplot(2,1,2),plot(v,alp-fv1); 
 
    
    figure()
    plot(yy)
toc
